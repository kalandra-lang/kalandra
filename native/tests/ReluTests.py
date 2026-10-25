"""ReLU conformance on all numeric types, CPU modes and optional physical GPU."""
import argparse
import subprocess
import sys
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--clang',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--gpu',action='store_true')
p.add_argument('--quick',action='store_true')
p.add_argument('--gpu-only',action='store_true')
p.add_argument('--arithmetic',choices=['hardware','software'],default='hardware')
a=p.parse_args();root=Path(__file__).resolve().parents[2];compiler=a.compiler.resolve();clang=a.clang.absolute();a.output.mkdir(parents=True,exist_ok=True)
types=('int8','int16','int32','int64','uint8','uint16','uint32','uint64','float32','float64')
variants=[('optimized','-O2',['-O2'])]
if not a.quick:
 variants += [('debug','-O0',['-O0']),('sanitized','--sanitize',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'])]
 if sys.platform!='win32':variants += [('thread-sanitized','--thread-sanitize',['-O1','-g','-fsanitize=thread','-fno-omit-frame-pointer'])]
def run(args):
 r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=240)
 assert r.returncode==0,(args,r.returncode,r.stdout,r.stderr)
 return r.stdout
# Invalid uses must fail without replacing an existing executable.
for label,expression in [('scalar','relu(2)'),('array','relu([1,2])'),('boolean','relu(true)'),('missing','relu()'),('extra','relu(tensor([1,2]),tensor([3,4]))')]:
 path=a.output/('invalid-'+label+'.k');binary=path.with_suffix('')
 path.write_text('fn main()->int:\n  '+expression+'\n');binary.write_bytes(b'preserve-existing-output')
 result=subprocess.run([str(compiler),str(path),'-o',str(binary)],capture_output=True,text=True,timeout=60)
 expected='relu expects 1 arguments' if label in ('missing','extra') else 'relu requires a tensor'
 assert result.returncode!=0 and expected in result.stderr,(label,result.stdout,result.stderr)
 assert binary.read_bytes()==b'preserve-existing-output',label
print('ReLU scalar/array/boolean/arity rejection and output preservation passed',flush=True)
for numeric in types:
 path=a.output/(numeric+'.k');path.write_text(f'fn activate(a:Tensor[{numeric},?])->Tensor[{numeric},?]:\n  relu(a)\nfn main()->int:\n  0\n')
 for variant,flag,flags in ([] if a.gpu_only else variants):
  binary=a.output/(numeric+'-'+variant);ir=binary.with_suffix('.ll')
  run([compiler,path,'--test-entry','activate','--arithmetic',a.arithmetic,'--emit-llvm',ir,flag,'-o',str(binary)+'-standalone'])
  runtime=compiler.parent/(('' if sys.platform=='win32' else 'lib')+'kalandra-native-runtime'+('-'+variant if 'sanitized' in variant else '')+('.lib' if sys.platform=='win32' else '.a'))
  run([clang,'-std=c++23','-ffp-contract=off','-Wno-override-module','-pthread',*flags,*(['-fms-runtime-lib=static'] if sys.platform=='win32' else []),'-DELEMENT='+('UInt'+numeric[4:] if numeric.startswith('uint') else 'Int'+numeric[3:] if numeric.startswith('int') else 'Float'+numeric[5:]),root/'native/tests/ReluCpu.cpp',ir,runtime,'-o',binary])
  print(variant+': '+run([binary]).strip(),flush=True)
 if a.gpu:
  path=a.output/(numeric+'-gpu.k');path.write_text(f'fn activate(a:Tensor[{numeric},?])->Tensor[{numeric},?]:\n  relu(a)\nfn main()->{numeric} ! TaskError:\n  try parallel:\n    values:Tensor[{numeric},3]=tensor([0,2,3])\n    gpu=devices.gpu()\n    a=try await transfer(values,gpu)\n    b=try await spawn activate(a) on gpu\n    c=try await transfer(b,devices.cpu())\n    reduceSum(c)\n')
  run([compiler,path,'--target','metal','--arithmetic',a.arithmetic,'-O2','-o',a.output/numeric])
if a.gpu:
 for variant,_,flags in variants:
  binary=a.output/('gpu-'+variant)
  runtime=compiler.parent/('libkalandra-task-core-optimized.a' if variant=='optimized' else 'libkalandra-native-runtime'+('-'+variant if 'sanitized' in variant else '')+'.a')
  run([clang,'-std=c++23','-pthread',*flags,root/'native/tests/ReluGpu.cpp',compiler.parent/'metal'/('libkalandra-metal-'+variant+'.a'),runtime,'-framework','Foundation','-framework','Metal','-o',binary])
  print(variant+': '+run([binary,a.output]).strip(),flush=True)
print('ReLU qualification passed',flush=True)

# Source-level shape preservation and placement, including a rank-zero tensor.
for numeric in ('float32','float64'):
 for rank in (0,3,8):
  dims=','+','.join('?' for _ in range(rank)) if rank else ''
  shape='['+','.join(['1']*(rank-1)+['2'])+']' if rank else '[]'
  values='[-3,2]' if rank else '[-3]'
  typed=f'Tensor[{numeric}{dims}]'
  body=f'fn activate(a:{typed})->{typed}:\n  relu(a)\n'
  path=a.output/(f'{numeric}-rank{rank}.k')
  path.write_text(body+f'fn main()->{numeric}:\n  a:{typed}=tensor({values},{shape})\n  reduceSum(activate(a))\n')
  binary=path.with_suffix('')
  run([compiler,path,'-O2','-o',binary])
  assert run([binary]).strip()==('2' if rank else '0')
  if a.gpu:
   path.write_text(body+f'fn main()->{numeric} ! TaskError:\n  try parallel:\n    values:{typed}=tensor({values},{shape})\n    gpu=devices.gpu()\n    a=try await transfer(values,gpu)\n    b=try await spawn activate(a) on gpu\n    c=try await transfer(b,devices.cpu())\n    reduceSum(c)\n')
   bundle=a.output/(f'{numeric}-gpu-rank{rank}')
   run([compiler,path,'--target','metal','--arithmetic',a.arithmetic,'-O2','-o',bundle])
   actual=int(run([bundle/'program',bundle]).strip(),16)
   expected=0 if not rank else 0x40000000 if numeric=='float32' else 0x4000000000000000
   assert actual==expected,(numeric,rank,actual,expected)
  print(numeric,'rank',rank,'source CPU/GPU' if a.gpu else 'source CPU','passed',flush=True)
