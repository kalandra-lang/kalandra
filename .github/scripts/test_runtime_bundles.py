"""Keep multi-target distributions deterministic and free of foreign compilers/SDKs."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'native/packaging'))
import RuntimeBundles as bundles
from CreateArchive import create_archive


class RuntimeBundleTests(unittest.TestCase):
    def fixture(self, root, platform):
        prefix = root / ('fixture-' + platform)
        prefix.mkdir()
        suffix = '.exe' if platform.startswith('windows-') else ''
        files = {'bin/kalandra' + suffix: b'host compiler ' + platform.encode(),
                 'lib/kalandra/toolchain/clang/include/stddef.h': b'compiler header',
                 'lib/kalandra/toolchain/clang/lib/builtins.lib': b'LLVM builtins',
                 'lib/kalandra/toolchain/clang/lib/asan.dll': b'foreign sanitizer DLL',
                 'lib/kalandra/native/libkalandra-native-runtime.a': b'normal runtime ' + platform.encode(),
                 'lib/kalandra/native/libkalandra-sanitized-runtime.a': b'excluded sanitizer',
                 'share/licenses/kalandra-toolchain/LLVM-LICENSE.txt': b'license'}
        for i in range(9): files[f'lib/kalandra/native/libkalandra-{i}-entry-debug.a'] = bytes([i])
        drivers = {}
        for kind, name in (('elf','ld.lld'),('macho','ld64.lld'),('coff','lld-link')):
            path = 'libexec/kalandra/' + name + suffix
            files[path] = b'host linker ' + platform.encode()
            drivers[kind] = path
        if platform == 'macos-arm64':
            files['share/kalandra-native-tools/macos-link-imports/interfaces/usr/lib/libSystem.tbd'] = b'own import declarations'
        for name, value in files.items():
            path = prefix / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(value)
        info = dict(format=1,version=(ROOT/'VERSION').read_text().strip(),platform=platform,
                    llvmVersion=json.loads((ROOT/'native/toolchain-proof/toolchain.json').read_text())['llvmVersion'],
                    crossLinkers=drivers)
        if platform == 'macos-arm64': info['minimumMacOS'] = '26.0'
        (prefix/bundles.META).parent.mkdir(parents=True,exist_ok=True)
        (prefix/bundles.META).write_text(json.dumps(info))
        bundles.inventory(prefix)
        return prefix

    def test_projection_preserves_runtime_bytes_but_excludes_foreign_tools_and_dynamic_libraries(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            source=self.fixture(root,'macos-arm64')
            target=root/'projected'
            bundles.project(source,target,'macos-arm64')
            bundles.verify(target)
            self.assertEqual((target/'lib/kalandra/native/libkalandra-native-runtime.a').read_bytes(),
                             (source/'lib/kalandra/native/libkalandra-native-runtime.a').read_bytes())
            self.assertFalse((target/'bin').exists())
            self.assertFalse((target/'libexec').exists())
            self.assertFalse(list(target.rglob('*.dll')))
            self.assertFalse((target/'lib/kalandra/native/libkalandra-sanitized-runtime.a').exists())
            self.assertEqual(json.loads((target/bundles.META).read_text())['minimumMacOS'],'26.0')

    def test_repack_all_hosts_contains_five_targets_with_identical_host_and_runtime_bytes(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);assets=root/'assets';assets.mkdir()
            originals={}
            for platform in bundles.PLATFORMS:
                source=self.fixture(root,platform)
                originals[platform]=source
                archive=assets/f'kalandra-native-cpu-standalone-{platform}.tar.gz'
                create_archive(source,archive)
                archive.with_name(archive.name+'.sha256').write_text(bundles.digest(archive)+'  '+archive.name+'\n')
            outputs=[]
            for repeat in ('one','two'):
                output=root/repeat
                bundles.assemble(assets,output)
                outputs.append(output)
            self.assertEqual({p.name:bundles.digest(p) for p in outputs[0].glob('*.tar.gz')},
                             {p.name:bundles.digest(p) for p in outputs[1].glob('*.tar.gz')})
            for platform in bundles.PLATFORMS:
                archive=outputs[0]/f'kalandra-native-cpu-standalone-{platform}.tar.gz'
                prefix=bundles.extract(archive,root/('verified-'+platform))
                suffix='.exe' if platform.startswith('windows-') else ''
                self.assertEqual((prefix/('bin/kalandra'+suffix)).read_bytes(),
                                 (originals[platform]/('bin/kalandra'+suffix)).read_bytes())
                self.assertEqual(set(json.loads((prefix/bundles.META).read_text())['bundledCpuTargets']),set(bundles.PLATFORMS))
                for target in bundles.PLATFORMS:
                    data=prefix/bundles.TARGETS/target
                    bundles.verify(data)
                    self.assertFalse((data/'bin').exists())
                    self.assertEqual((data/'lib/kalandra/native/libkalandra-native-runtime.a').read_bytes(),
                                     (originals[target]/'lib/kalandra/native/libkalandra-native-runtime.a').read_bytes())

    def test_corrupt_sources_and_mismatched_target_metadata_are_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=self.fixture(root,'linux-arm64')
            archive=root/'source.tar.gz';create_archive(source,archive)
            archive.with_name(archive.name+'.sha256').write_text('0'*64+'  source.tar.gz\n')
            with self.assertRaisesRegex(ValueError,'Source archive checksum mismatch'):
                bundles.extract(archive,root/'bad')
            with self.assertRaisesRegex(ValueError,'version/platform/LLVM mismatch'):
                bundles.project(source,root/'wrong','linux-x86_64')
            (source/'bin/kalandra').write_bytes(b'tampered')
            with self.assertRaisesRegex(ValueError,'checksum mismatch'):
                bundles.verify(source)


if __name__ == '__main__': unittest.main()
