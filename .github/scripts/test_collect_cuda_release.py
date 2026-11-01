"""CUDA-specific release identity gates; archive safety is shared with Vulkan."""
from contextlib import ExitStack
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import collect_vulkan_release as collector

ARCHIVE='kalandra-native-cuda-standalone-linux-x86_64.tar.gz'
ARTIFACT='native-cuda-package-linux-x86_64'

class CudaReleaseTests(unittest.TestCase):
    def api(self,path):
        if '/workflows/' in path:
            self.assertIn('/native-cuda.yml/',path)
            return {'workflow_runs':[{'id':3,'head_sha':'commit','status':'completed','conclusion':'success',
                'event':'push','head_branch':'v1.2.3','head_repository':{'full_name':'example/project'},
                'html_url':'https://github.com/example/project/actions/runs/3'}]}
        return {'artifacts':[{'id':33,'name':ARTIFACT,'expired':False}]}

    def workspace(self):
        stack=ExitStack();self.addCleanup(stack.close)
        root=Path(stack.enter_context(tempfile.TemporaryDirectory())).resolve()
        before=Path.cwd();os.chdir(root);stack.callback(os.chdir,before)
        stack.enter_context(patch.dict(os.environ,GH_REPO='example/project',GITHUB_SHA='commit',RELEASE_TAG='v1.2.3'))
        (root/'assets').mkdir()
        return root,stack

    def test_cuda_selects_own_workflow_artifact_and_receipt(self):
        root,stack=self.workspace()
        def download(command,stdout,check):
            with zipfile.ZipFile(stdout,'w') as archive:
                archive.writestr(ARCHIVE,b'cuda')
                import hashlib
                archive.writestr(ARCHIVE+'.sha256',hashlib.sha256(b'cuda').hexdigest()+'  '+ARCHIVE+'\n')
        def attach(source,runtimes,output):
            self.assertEqual(source.name,ARCHIVE)
            output.mkdir();(output/ARCHIVE).write_bytes(b'five CPU targets with CUDA')
            (output/(ARCHIVE+'.sha256')).write_text(collector.digest(output/ARCHIVE)+'  '+ARCHIVE+'\n')
        stack.enter_context(patch.object(collector,'api',side_effect=self.api))
        stack.enter_context(patch.object(collector.subprocess,'run',side_effect=download))
        composition=stack.enter_context(patch.object(collector,'attach',side_effect=attach))
        collector.collect(root/'runtimes',root/'assets',backend='cuda')
        self.assertEqual(composition.call_count,2)
        self.assertEqual({p.name for p in (root/'assets').iterdir()},{ARCHIVE,ARCHIVE+'.sha256'})
        self.assertEqual(json.loads((root/'cuda-qualification.json').read_text())['archive'],ARCHIVE)
        self.assertFalse((root/'vulkan-qualification.json').exists())

    def test_failed_fork_wrong_revision_tag_or_backend_artifact_never_downloads(self):
        for key,value in [('conclusion','failure'),('head_sha','other'),('head_branch','develop'),
                          ('event','pull_request'),('head_repository',{'full_name':'fork/project'}),('artifact','native-vulkan-package-linux-x86_64')]:
            with self.subTest(key=key),tempfile.TemporaryDirectory() as directory:
                def api(path):
                    data=self.api(path)
                    if key=='artifact':
                        if '/workflows/' not in path:data['artifacts'][0]['name']=value
                    elif '/workflows/' in path:data['workflow_runs'][0][key]=value
                    return data
                with patch.dict(os.environ,GH_REPO='example/project',GITHUB_SHA='commit',RELEASE_TAG='v1.2.3'),patch.object(collector,'api',side_effect=api),patch.object(collector.subprocess,'run') as download:
                    with self.assertRaises(RuntimeError):collector.collect(Path('runtimes'),Path(directory),backend='cuda')
                    download.assert_not_called()

    def test_cuda_tag_trigger_and_release_qualification_are_required(self):
        root=Path(__file__).resolve().parents[2]
        workflow=(root/'.github/workflows/native-cuda.yml').read_text()
        release=(root/'.github/workflows/native-release.yml').read_text()
        from native_release_assets import ARCHIVES
        self.assertIn(ARCHIVE,ARCHIVES)
        import re
        for text in (workflow,release):
            for script in re.findall(r'python3 (native/[\w/.-]*cuda[\w/.-]*\.py)',text):
                self.assertTrue((root/script).is_file(),script)
        self.assertIn('tags: ["v*"]',workflow)
        self.assertIn('  qualify-cuda:',release)
        self.assertIn(ARCHIVE,release)
        self.assertIn('qualify-vulkan, qualify-cuda, notarize]',release)
        self.assertIn("if: startsWith(github.ref, 'refs/tags/')",release.split('  publish:\n',1)[1])

if __name__=='__main__':unittest.main()
