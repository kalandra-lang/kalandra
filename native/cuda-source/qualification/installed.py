"""Qualify production-pinned CUDA archives after relocation; installs nothing.

GPU execution is opt-in. Hosted checks compile real GPU programs without
substituting a CPU device. Missing owned resources cannot fall back to the build.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tarfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--archive',type=Path,required=True)
p.add_argument('--root',type=Path,required=True)
p.add_argument('--execute',action='store_true')
p.add_argument('--bundled-targets',action='store_true')
a=p.parse_args();a.archive=a.archive.resolve();a.root=a.root.resolve()
a.root.mkdir(parents=True,exist_ok=False)
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
assert a.archive.with_suffix('.gz.sha256').read_text().split()[0]==digest(a.archive)
name='kalandra-native-cuda-standalone'
prefix=a.root/'relocated prefix λ'/name
with tarfile.open(a.archive) as archive:
    assert all(m.isdir() or m.isfile() for m in archive.getmembers())
    assert all(Path(m.name).parts[0]==name and '..' not in Path(m.name).parts for m in archive.getmembers())
    archive.extractall(prefix.parent,filter='data')
listed={}
for line in (prefix/'SHA256SUMS').read_text().splitlines():
    expected,relative=line.split('  ',1)
    assert relative not in listed and (prefix/relative).resolve().is_relative_to(prefix)
    assert digest(prefix/relative)==expected,relative
    listed[relative]=expected
assert {str(path.relative_to(prefix)) for path in prefix.rglob('*') if path.is_file()}==set(listed)|{'SHA256SUMS'}
metadata=json.loads((prefix/'share/kalandra-native-cuda/toolchain.json').read_text())
assert metadata['format']==1 and metadata['llvmVersion']=='23.1.3'
assert metadata['cudaSDKIncluded'] is False and metadata['optimizedMatmulCatalog']==1
if a.bundled_targets:
    cpu=json.loads((prefix/'share/kalandra-native-tools/toolchain.json').read_text())
    expected={'linux-x86_64','linux-arm64','windows-x86_64','windows-arm64','macos-arm64'}
    assert set(cpu['bundledCpuTargets'])==expected
    assert {path.name for path in (prefix/'lib/kalandra/targets').iterdir()}==expected
for relative in listed:
    base=Path(relative).name.lower()
    assert not any(x in base for x in ('libcuda.so','libcudart','libcublas','libnvrtc')),relative
    assert Path(relative).name not in ('cuda.h','cuda_runtime.h'),relative
    assert not base.endswith(('.cu','.raw.ptx')),relative
repo=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(repo/'native/packaging'))
from BundleELF import audit
from VersionCheck import verify_package_version
verify_package_version(prefix);audit(prefix)
compiler=prefix/'bin/kalandra'
source=a.root/'cpu.k';source.write_text('fn main()->int32:\n  42\n')
subprocess.run([compiler,source,'-o',a.root/'cpu'],check=True,timeout=90)
assert subprocess.check_output([a.root/'cpu'],text=True,timeout=30)=='42\n'
subprocess.run([sys.executable,Path(__file__).with_name('applications.py'),
    '--build',a.root,'--compiler',compiler,'--output',a.root/'applications',
    '--execute' if a.execute else '--compile-only'],check=True,timeout=1200)
subprocess.run([sys.executable,Path(__file__).parents[2]/'cuda/tests/loader.py',
    '--application',a.root/'applications/hardware/float32/program',
    '--cxx',prefix/'libexec/kalandra/clang++',
    '--resource-dir',prefix/'lib/kalandra/toolchain/clang'],check=True,timeout=90)
# Use different source constructs to require the precise missing resource.
transform=a.root/'transpose.k'
transform.write_text('''fn work(v:Tensor[float32,?,?])->Tensor[float32,?,?]:
  transpose(v)
fn main()->Tensor[float32,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    a:Tensor[float32,2,2]=tensor([[1,2],[3,4]])
    x=try await transfer(a,gpu)
    y=try await spawn work(x) on gpu
    try await transfer(y,devices.cpu())
''')
placed=repo/'native/metal-source/placed.k'
matrix=repo/'native/metal-source/matmul.k'
checks=[('libexec/kalandra/clang++',placed),('libexec/kalandra/kalandra-cuda-lower',placed),
    ('lib/kalandra/native/libkalandra-cuda.a',placed),
    ('lib/kalandra/native/headers/cuda',placed),
    ('lib/kalandra/native/headers/gpu-runtime/ProgramDecl.inc',placed),
    ('lib/kalandra/toolchain/clang/include',placed),
    ('lib/kalandra/native/cuda/transforms/transpose-float32.ptx',transform),
    ('lib/kalandra/native/cuda/kernels/matmul-catalog.ptx',matrix)]
for index,(relative,source) in enumerate(checks):
    owned=prefix/relative;saved=owned.with_name(owned.name+'.qualification-hidden');owned.rename(saved)
    try:
        output=a.root/('missing-'+str(index))
        run=subprocess.run([compiler,'--target','cuda','--matmul-policy','optimized',source,'-O2','-o',output],capture_output=True,text=True,timeout=90)
        assert run.returncode==1 and not (output/'program').exists(),(relative,run)
    finally:saved.rename(owned)
print('PASS: relocated CUDA/CPU compiler, complete source programs, SDK-free ELF closure and eight owned-resource failure gates')
