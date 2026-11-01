"""Build frozen numerical-library benchmark clients; never installs dependencies."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--cxx',type=Path,default=Path('/usr/bin/clang++'))
p.add_argument('--blas',type=Path,default=Path('/lib64/libopenblas.so.0'))
a=p.parse_args()
repo=Path(__file__).resolve().parents[3]
a.build=a.build.resolve();a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=False)
compiler=a.build/'native/kalandra'
def run(command):
    subprocess.run(list(map(str,command)),check=True,timeout=240)
source=a.output/'cpu.k'
source.write_text('''fn product(a:Tensor[float32,?,?],b:Tensor[float32,?,?])->Tensor[float32,?,?]:
  matmul(a,b)
fn main()->int32:
  0
''')
ir=a.output/'cpu.ll'
run([compiler,source,'--test-entry','product','--arithmetic','hardware','--emit-llvm',ir,'-O2','-o',a.output/'cpu-standalone'])
assert 'define void @_mlir_ciface_kalandra_test_entry(ptr' in ir.read_text(), 'Check tensor-result C ABI before linking client'
source=a.output/'gpu.k'
source.write_text('''fn product(a:Tensor[float32,?,?],b:Tensor[float32,?,?])->Tensor[float32,?,?]:
  matmul(a,b)
fn main()->Tensor[float32,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    a:Tensor[float32,2,2]=tensor([[1,2],[3,4]])
    b:Tensor[float32,2,2]=tensor([[2,1],[0,3]])
    x=try await transfer(a,gpu)
    y=try await transfer(b,gpu)
    result=try await spawn product(x,y) on gpu
    try await transfer(result,devices.cpu())
''')
for policy in ('fixed-tree','optimized'):
    run([compiler,'--target','vulkan',source,'--arithmetic','hardware','--matmul-policy',policy,'-O2','-o',a.output/policy])
    assert 'matmul_policy='+policy in (a.output/policy/'manifest.txt').read_text().splitlines()
cpp=repo/'native/vulkan/benchmarks/Matmul.cpp'
obj=a.output/'cpu.o'
run([a.cxx,'-O2','-c',ir,'-o',obj])
runtime=a.build/'native'
archives=[runtime/'vulkan'/('libkalandra-vulkan'+suffix+'.a') for suffix in ('-language','-async','-program','')]
archives.append(runtime/'libkalandra-native-runtime.a')
run([a.cxx,'-std=c++23','-O3','-ffp-contract=off','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers',
     '-I'+str(repo/'native/vulkan'),'-I'+str(repo/'native/include'),cpp,obj,*archives,'-ldl','-pthread','-o',a.output/'benchmark'])
def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
files=[compiler,a.cxx.resolve(),a.blas.resolve(),cpp,ir,obj,*archives,a.output/'benchmark',Path(__file__).resolve(),
       repo/'native/vulkan/benchmarks/measure.py',
       *a.output.glob('*.k'),*a.output.glob('*/kernel*.spv'),*a.output.glob('*/manifest.txt')]
record={'format':1,'scope':'numerical-library API comparison; no source-level loop or language-speed ranking',
        'arithmetic':'float32 hardware; CPU fixed-tree, GPU explicit fixed-tree or optimized full-float32',
        'gpuBudgetBytes':64*1024*1024,'workerThreads':1,
        'nativeCpuObjectOptimization':'-O2; no LTO', 'adapterOptimization':'-O3',
        'compilerVersion':subprocess.check_output([compiler,'--version'],text=True).strip(),
        'cxxVersion':subprocess.check_output([a.cxx,'--version'],text=True).splitlines()[0],
        'blas':str(a.blas.resolve()),'binary':str(a.output/'benchmark'),
        'hashes':{str(path):digest(path) for path in files},
        'system':subprocess.check_output(['uname','-a'],text=True).strip(),
        'cpu':subprocess.check_output(['lscpu'],text=True)}
(a.output/'manifest.json').write_text(json.dumps(record,indent=2)+'\n')
print('Prepared frozen benchmark clients; no acceptance timings collected',flush=True)
