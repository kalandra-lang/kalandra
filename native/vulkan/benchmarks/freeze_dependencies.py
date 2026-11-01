"""Retain exact static build dependencies before rebuilding a candidate.

Executed binaries, shaders, adapters and the live system BLAS keep their original
hash checks. Only build-time compiler/archive inputs are relocated byte-for-byte;
original manifests and source snapshots are retained for provenance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--dependency-root',type=Path,required=True)
p.add_argument('--source-root',type=Path,required=True)
a=p.parse_args();a.build=a.build.resolve();dependency=a.dependency_root.resolve()
manifest=a.build/'manifest.json'
record=json.loads(manifest.read_text())
def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
assert not (a.build/'manifest.original.json').exists()
for name,expected in record['hashes'].items():assert digest(Path(name))==expected,name
shutil.copy2(manifest,a.build/'manifest.original.json')
root=a.build/'frozen-dependencies';root.mkdir()
replaced={}
for index,(name,expected) in enumerate(list(record['hashes'].items())):
    source=Path(name)
    if source.is_relative_to(dependency):
        target=root/(str(index)+'-'+source.name)
        shutil.copy2(source,target);assert digest(target)==expected
        del record['hashes'][name];record['hashes'][str(target)]=expected
        replaced[str(target)]={'original':name,'sha256':expected}
sources=root/'source';sources.mkdir()
for source in sorted(a.source_root.resolve().glob('*')):
    if source.suffix in ('.cpp','.h'):
        target=sources/source.name;shutil.copy2(source,target)
        record['hashes'][str(target)]=digest(target)
record['frozenBuildDependencies']=replaced
record['originalManifestSHA256']=digest(a.build/'manifest.original.json')
record['freezeImplementationSHA256']=digest(Path(__file__).resolve())
manifest.write_text(json.dumps(record,indent=2)+'\n')
print('Frozen',len(replaced),'exact build-time inputs; executable/shader/library checks preserved',flush=True)
