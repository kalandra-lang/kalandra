import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import collect_vulkan_release as collector


class VulkanReleaseTests(unittest.TestCase):
    def api(self, path):
        if '/workflows/' in path:
            return {'workflow_runs': [{'id': 3, 'head_sha': 'commit', 'status': 'completed',
                'conclusion': 'success', 'event': 'push', 'head_branch': 'v1.2.3',
                'head_repository': {'full_name': 'example/project'}, 'html_url': 'https://github.com/example/project/actions/runs/3'}]}
        return {'artifacts': [{'id': 33, 'name': collector.ARTIFACT, 'expired': False}]}

    def download(self, command, stdout, check):
        from hashlib import sha256
        data = b'qualified fixture'
        with zipfile.ZipFile(stdout, 'w') as archive:
            archive.writestr(collector.ARCHIVE, data)
            archive.writestr(collector.ARCHIVE+'.sha256', sha256(data).hexdigest()+'  '+collector.ARCHIVE+'\n')

    def attach(self, source, runtimes, output):
        output.mkdir()
        archive = output/collector.ARCHIVE
        archive.write_bytes(b'five runtime targets attached')
        archive.with_name(archive.name+'.sha256').write_text(collector.digest(archive)+'  '+archive.name+'\n')

    def workspace(self, check):
        with tempfile.TemporaryDirectory() as directory:
            before = Path.cwd()
            try:
                os.chdir(directory)
                Path('release-assets').mkdir()
                with patch.dict(os.environ, GH_REPO='example/project', GITHUB_SHA='commit', RELEASE_TAG='v1.2.3'):
                    check()
            finally:
                os.chdir(before)

    def test_collects_exact_tag_and_composes_twice_before_publishing_assets(self):
        def check():
            with patch.object(collector, 'api', side_effect=self.api), patch.object(collector.subprocess, 'run', side_effect=self.download), patch.object(collector, 'attach', side_effect=self.attach) as attach:
                collector.collect(Path('runtime-data.tar.gz'), Path('release-assets'))
            self.assertEqual(attach.call_count, 2)
            self.assertEqual({p.name for p in Path('release-assets').iterdir()}, {collector.ARCHIVE, collector.ARCHIVE+'.sha256'})
            self.assertEqual(json.loads(Path('vulkan-qualification.json').read_text())['commit'], 'commit')
        self.workspace(check)

    def test_failed_wrong_commit_wrong_tag_fork_or_pr_blocks_publication(self):
        for key, value in [('conclusion', 'failure'), ('head_sha', 'other'), ('head_branch', 'native'),
                           ('head_branch', 'v1.2.4'), ('event', 'pull_request'),
                           ('head_repository', {'full_name': 'fork/project'})]:
            def api(path):
                response = self.api(path)
                if '/workflows/' in path: response['workflow_runs'][0][key] = value
                return response
            def check():
                with patch.object(collector, 'api', side_effect=api), patch.object(collector.subprocess, 'run') as download:
                    with self.assertRaisesRegex(RuntimeError, 'exact release tag/revision'):
                        collector.collect(Path('runtime-data'), Path('release-assets'))
                    download.assert_not_called()
            with self.subTest(key=key, value=value): self.workspace(check)

    def test_rehearsal_accepts_only_own_revision(self):
        def api(path):
            response = self.api(path)
            if '/workflows/' in path: response['workflow_runs'][0].update(event='pull_request', head_branch='feature')
            return response
        def check():
            with patch.object(collector, 'api', side_effect=api), patch.object(collector.subprocess, 'run', side_effect=self.download), patch.object(collector, 'attach', side_effect=self.attach):
                collector.collect(Path('runtime-data'), Path('release-assets'), rehearsal=True)
        self.workspace(check)

    def test_pending_qualification_waits_before_collecting(self):
        calls = 0
        def api(path):
            nonlocal calls
            response = self.api(path)
            if '/workflows/' in path:
                calls += 1
                if calls == 1: response['workflow_runs'][0].update(status='in_progress', conclusion=None)
            return response
        def check():
            with patch.object(collector, 'api', side_effect=api), patch.object(collector.subprocess, 'run', side_effect=self.download), patch.object(collector, 'attach', side_effect=self.attach), patch.object(collector.time, 'sleep') as sleep:
                collector.collect(Path('runtime-data'), Path('release-assets'))
                sleep.assert_called_once_with(30)
        self.workspace(check)

    def test_missing_expired_or_duplicate_artifact_blocks(self):
        for artifacts in ([], [{'id': 33, 'name': collector.ARTIFACT, 'expired': True}],
                          [{'id': n, 'name': collector.ARTIFACT, 'expired': False} for n in (33, 34)]):
            def api(path):
                response = self.api(path)
                if '/workflows/' not in path: response['artifacts'] = artifacts
                return response
            def check():
                with patch.object(collector, 'api', side_effect=api), patch.object(collector.subprocess, 'run') as download:
                    with self.assertRaises(RuntimeError): collector.collect(Path('runtime-data'), Path('release-assets'))
                    download.assert_not_called()
            with self.subTest(artifacts=artifacts): self.workspace(check)

    def test_unsafe_members_and_bad_checksum_block_before_composition(self):
        for unsafe in (True, False):
            def download(command, stdout, check):
                with zipfile.ZipFile(stdout, 'w') as archive:
                    if unsafe: archive.writestr('../escape', 'bad')
                    else:
                        archive.writestr(collector.ARCHIVE, b'changed')
                        archive.writestr(collector.ARCHIVE+'.sha256', 'wrong  '+collector.ARCHIVE)
            def check():
                with patch.object(collector, 'api', side_effect=self.api), patch.object(collector.subprocess, 'run', side_effect=download), patch.object(collector, 'attach') as attach:
                    with self.assertRaises(RuntimeError): collector.collect(Path('runtime-data'), Path('release-assets'))
                    attach.assert_not_called()
                    self.assertFalse(list(Path('release-assets').iterdir()))
                    self.assertFalse(Path('vulkan-qualification.json').exists())
            with self.subTest(unsafe=unsafe): self.workspace(check)

    def test_nonreproducible_composition_does_not_export_assets(self):
        def attach(source, runtimes, output):
            self.attach(source, runtimes, output)
            if output.name == 'repeated': (output/collector.ARCHIVE).write_bytes(b'different')
        def check():
            with patch.object(collector, 'api', side_effect=self.api), patch.object(collector.subprocess, 'run', side_effect=self.download), patch.object(collector, 'attach', side_effect=attach):
                with self.assertRaisesRegex(RuntimeError, 'not reproducible'):
                    collector.collect(Path('runtime-data'), Path('release-assets'))
                self.assertFalse(list(Path('release-assets').iterdir()))
        self.workspace(check)

    def test_rehearsal_rejects_fork_or_wrong_revision(self):
        for key, value in [('head_sha', 'other'), ('head_repository', None),
                           ('head_repository', {'full_name': 'fork/project'})]:
            def api(path):
                response = self.api(path)
                if '/workflows/' in path: response['workflow_runs'][0][key] = value
                return response
            def check():
                with patch.object(collector, 'api', side_effect=api), patch.object(collector.subprocess, 'run') as download:
                    with self.assertRaises(RuntimeError):
                        collector.collect(Path('runtime-data'), Path('release-assets'), rehearsal=True)
                    download.assert_not_called()
            with self.subTest(key=key): self.workspace(check)

    def test_publication_requires_tag(self):
        def check():
            with patch.dict(os.environ, RELEASE_TAG=''), patch.object(collector, 'api') as api:
                with self.assertRaisesRegex(RuntimeError, 'exact release tag'):
                    collector.collect(Path('runtime-data'), Path('release-assets'))
                api.assert_not_called()
        self.workspace(check)

    def test_invalid_runtime_data_blocks_export(self):
        def check():
            with patch.object(collector, 'api', side_effect=self.api), patch.object(collector.subprocess, 'run', side_effect=self.download), patch.object(collector, 'attach', side_effect=ValueError('CPU target runtime version mismatch')):
                with self.assertRaisesRegex(ValueError, 'version mismatch'):
                    collector.collect(Path('runtime-data'), Path('release-assets'))
                self.assertFalse(list(Path('release-assets').iterdir()))
                self.assertFalse(Path('vulkan-qualification.json').exists())
        self.workspace(check)

    def test_tag_trigger_and_publish_dependency_remain_explicit(self):
        root = Path(__file__).resolve().parents[2]
        vulkan = (root/'.github/workflows/native-vulkan.yml').read_text()
        release = (root/'.github/workflows/native-release.yml').read_text()
        from native_release_assets import ARCHIVES
        self.assertIn(collector.ARCHIVE, ARCHIVES)
        self.assertIn(collector.ARCHIVE, release)
        self.assertIn('tags: ["v*"]', vulkan)
        self.assertIn('needs: [build, bundle-targets, qualify-standalone, qualify-vulkan, qualify-cuda, notarize]', release)
        publish = release.split('  publish:\n', 1)[1]
        self.assertIn("if: startsWith(github.ref, 'refs/tags/')", publish)

    def test_real_archive_extraction_canonicalizes_parent_alias(self):
        from RuntimeBundles import create_archive, extract, inventory, verify
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            payload = root/'package'
            payload.mkdir()
            (payload/'linker').write_bytes(b'owned linker fixture')
            inventory(payload)
            archive = root/'fixture.tar.gz'
            create_archive(payload, archive)
            archive.with_name(archive.name+'.sha256').write_text(collector.digest(archive)+'  '+archive.name+'\n')
            target = root/'target'
            target.mkdir()
            alias = root/'alias'
            try:
                alias.symlink_to(target, target_is_directory=True)
            except OSError:
                self.skipTest('Creating directory symlinks requires host privileges')
            prefix = extract(archive, alias/'extracted')
            self.assertEqual(prefix, target/'extracted/package')
            self.assertEqual(prefix, prefix.resolve())
            self.assertTrue((prefix/'linker').resolve().is_relative_to(prefix))
            verify(prefix)

if __name__ == '__main__': unittest.main()
