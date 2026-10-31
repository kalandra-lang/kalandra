"""Only pinned host linkers may enter a standalone compiler package."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'native/packaging'))
import CrossLinkers
import BundleELF
import BundleMachO
import BundlePE


class CrossLinkersTest(unittest.TestCase):
    def test_every_platform_packager_stages_host_linkers(self):
        for packager in (BundleELF, BundleMachO, BundlePE):
            self.assertIs(packager.stage_cross_linkers, CrossLinkers.stage)

    def test_host_suffix_pinned_version_and_driver_bytes(self):
        for platform in ('macos-arm64', 'linux-x86_64', 'windows-arm64'):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory() as folder:
                root = Path(folder); prefix = root / 'package'; llvm = root / 'llvm'
                metadata = prefix / 'share/kalandra-native-tools/toolchain.json'
                metadata.parent.mkdir(parents=True)
                metadata.write_text(json.dumps(dict(platform=platform)))
                suffix = '.exe' if platform.startswith('windows-') else ''
                source = llvm / ('bin/lld-link' + suffix)
                source.parent.mkdir(parents=True); source.write_bytes(b'host lld')
                with patch.dict(CrossLinkers.os.environ, KALANDRA_CROSS_LLD_ROOT=str(source.parent)):
                    with patch.object(CrossLinkers.subprocess, 'check_output', return_value='LLD wrong version'):
                        with self.assertRaisesRegex(RuntimeError, 'pinned toolchain'): CrossLinkers.stage(prefix, llvm)
                    version = json.loads((CrossLinkers.ROOT / 'native/toolchain-proof/toolchain.json').read_text())['llvmVersion']
                    with patch.object(CrossLinkers.subprocess, 'check_output', return_value='LLD ' + version):
                        seeds = CrossLinkers.stage(prefix, llvm)
                self.assertEqual(len(seeds), 3)
                for original, target in seeds:
                    self.assertEqual(original, source.resolve())
                    self.assertEqual(target.read_bytes(), source.read_bytes())
                    self.assertEqual(target.name.endswith('.exe'), bool(suffix))
                self.assertEqual(set(json.loads(metadata.read_text())['crossLinkers']), {'elf', 'macho', 'coff'})
