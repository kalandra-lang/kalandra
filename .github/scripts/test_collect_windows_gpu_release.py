from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import collect_windows_gpu_release as collector
import collect_vulkan_release as identity


class WindowsGpuReleaseTests(unittest.TestCase):
    def workspace(self, architecture='arm64', mutate=None):
        stack = ExitStack()
        self.addCleanup(stack.close)
        root = Path(stack.enter_context(tempfile.TemporaryDirectory())).resolve()
        before = Path.cwd()
        os.chdir(root)
        stack.callback(os.chdir, before)
        stack.enter_context(patch.dict(os.environ, GH_REPO='example/project', GITHUB_SHA='commit', RELEASE_TAG='v1.2.3'))
        (root/'assets').mkdir()
        def download(command, stdout, check):
            members = {}
            for backend in ('cuda', 'vulkan'):
                name = f'kalandra-native-{backend}-standalone-windows-{architecture}.tar.gz'
                data = backend.encode()
                sha = hashlib.sha256(data).hexdigest()
                prefix = f'windows-gpu/native-{backend}-standalone/'
                members[prefix+name] = data
                members[prefix+name+'.sha256'] = sha+'  '+name+'\n'
                members[f'installed-{backend}/qualification.json'] = json.dumps(dict(
                    backend=backend, architecture=architecture, archiveSHA256=sha,
                    installedCompiler=True, cpuExecution=True, gpuExecution=False,
                    performanceQualified=False, bundledTargets=False,
                    gpuSourceBuilds=51 if backend == 'cuda' else 15))
            if mutate:
                mutate(members)
            with zipfile.ZipFile(stdout, 'w') as archive:
                for name, data in members.items():
                    archive.writestr(name, data)
        def attach(source, runtimes, output):
            output.mkdir()
            target = output/source.name
            target.write_bytes(b'reproducibly bundled')
            (output/(source.name+'.sha256')).write_text(collector.digest(target)+'  '+source.name+'\n')
        select = stack.enter_context(patch.object(collector, 'qualified_artifact', return_value=(
            {'html_url': 'https://github.com/example/project/actions/runs/3'}, {'id': 33})))
        stack.enter_context(patch.object(collector.subprocess, 'run', side_effect=download))
        compose = stack.enter_context(patch.object(collector, 'attach', side_effect=attach))
        return root, select, compose

    def test_collects_both_backends_both_architectures_after_double_composition(self):
        for architecture in ('x86_64', 'arm64'):
            with self.subTest(architecture=architecture):
                root, select, compose = self.workspace(architecture)
                collector.collect(Path('runtimes'), root/'assets', architecture)
                self.assertEqual(compose.call_count, 4)
                self.assertEqual(len(list((root/'assets').iterdir())), 4)
                select.assert_called_once_with('example/project', 'commit', False, platform='windows-'+architecture)

    def test_modified_report_checksum_missing_or_unexpected_member_reject_before_composition(self):
        def report(members):
            name = 'installed-cuda/qualification.json'
            data = json.loads(members[name]); data['gpuExecution'] = True
            members[name] = json.dumps(data)
        def checksum(members):
            name = next(name for name in members if name.endswith('.sha256'))
            members[name] = 'incorrect'
        def missing(members):
            del members['installed-vulkan/qualification.json']
        def unexpected(members):
            members['../escape'] = 'unsafe'
        for mutation in (report, checksum, missing, unexpected):
            with self.subTest(mutation=mutation.__name__):
                root, _, compose = self.workspace(mutate=mutation)
                with self.assertRaises(RuntimeError):
                    collector.collect(Path('runtimes'), root/'assets', 'arm64')
                compose.assert_not_called()
                self.assertEqual(list((root/'assets').iterdir()), [])

    def test_compile_evidence_is_accepted_without_exporting_it(self):
        def diagnostics(members):
            for backend in ('cuda', 'vulkan'):
                members[f'installed-{backend}/compile-jobs.json'] = '{"suites": []}'
                members[f'installed-{backend}/compile-logs/generators.log'] = 'qualified'
        root, _, compose = self.workspace(mutate=diagnostics)
        collector.collect(Path('runtimes'), root/'assets', 'arm64')
        self.assertEqual(compose.call_count, 4)
        self.assertEqual(len(list((root/'assets').iterdir())), 4)
        self.assertFalse(list(root.glob('*.log')))
        self.assertFalse(list(root.glob('compile-jobs.json')))

    def test_unknown_diagnostic_locations_and_extensions_reject(self):
        for name in ('other/compile-jobs.json', 'installed-cuda/extra.json',
                     'installed-cuda/compile-logs/program.exe',
                     'installed-vulkan/compile-logs/nested/output.log',
                     'installed-cuda/compile-logs/../escape.log'):
            with self.subTest(name=name):
                root, _, compose = self.workspace(mutate=lambda members: members.update({name: 'unexpected'}))
                with self.assertRaises(RuntimeError):
                    collector.collect(Path('runtimes'), root/'assets', 'arm64')
                compose.assert_not_called()
                self.assertEqual(list((root/'assets').iterdir()), [])

    def test_nonreproducible_second_backend_exports_nothing(self):
        root, _, compose = self.workspace()
        original = compose.side_effect
        def attach(source, runtimes, output):
            original(source, runtimes, output)
            if output.name == 'repeated-vulkan':
                (output/source.name).write_bytes(b'different')
        compose.side_effect = attach
        with self.assertRaisesRegex(RuntimeError, 'not reproducible'):
            collector.collect(Path('runtimes'), root/'assets', 'arm64')
        self.assertEqual(list((root/'assets').iterdir()), [])

    def test_exact_tag_platform_and_repository_identity(self):
        for architecture in ('x86_64', 'arm64'):
            for invalid in (None, 'tag', 'commit', 'repository', 'event'):
                with self.subTest(architecture=architecture, invalid=invalid):
                    run = dict(id=3, head_sha='commit', status='completed', conclusion='success',
                               event='push', head_branch='v1.2.3', head_repository={'full_name':'example/project'})
                    if invalid == 'tag': run['head_branch'] = 'develop'
                    if invalid == 'commit': run['head_sha'] = 'other'
                    if invalid == 'repository': run['head_repository'] = {'full_name':'fork/project'}
                    if invalid == 'event': run['event'] = 'pull_request'
                    def api(path):
                        if '/workflows/' in path:
                            self.assertIn('/native-windows-gpu.yml/', path)
                            return {'workflow_runs': [run]}
                        return {'artifacts': [dict(id=33, name='native-windows-gpu-packages-'+architecture, expired=False)]}
                    with patch.dict(os.environ, RELEASE_TAG='v1.2.3'), patch.object(identity, 'api', side_effect=api):
                        if invalid:
                            with self.assertRaises(RuntimeError):
                                identity.qualified_artifact('example/project', 'commit', platform='windows-'+architecture)
                        else:
                            self.assertEqual(identity.qualified_artifact('example/project','commit',platform='windows-'+architecture)[1]['id'],33)

    def test_release_requires_windows_archives_tag_builds_and_final_qualifier(self):
        from native_release_assets import ARCHIVES
        root = Path(__file__).resolve().parents[2]
        for backend in ('cuda','vulkan'):
            for architecture in ('x86_64','arm64'):
                self.assertIn(f'kalandra-native-{backend}-standalone-windows-{architecture}.tar.gz', ARCHIVES)
        self.assertIn('tags: ["v*"]', (root/'.github/workflows/native-windows-gpu.yml').read_text())
        release = (root/'.github/workflows/native-release.yml').read_text()
        self.assertIn('qualify-windows-gpu, notarize]', release)
        self.assertIn('windows_installed.py --backend $backend --bundled-targets', release)


if __name__ == '__main__':
    unittest.main()
