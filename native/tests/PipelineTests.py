"""Pure float32 projection/bias/ReLU pipeline numerical and lease conformance."""
import argparse
from pathlib import Path
import subprocess
import json
import shutil
import struct
p=argparse.ArgumentParser();p.add_argument('--compiler',type=Path,required=True);p.add_argument('--clang',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[2];compiler=a.compiler.resolve();clang=a.clang.absolute();llvm=clang.parent.parent;a.output.mkdir(parents=True,exist_ok=True)
def run(args):
 r=subprocess.run(list(map(str,args)),text=True,capture_output=True,timeout=240)
 assert r.returncode==0,(args,r.returncode,r.stdout,r.stderr)
 return r.stdout
bundle=a.output/'bundle'
run([compiler,root/'native/benchmarks/Pipeline.k','--target','metal','--arithmetic','hardware','-O2','-o',bundle])
assert 'kalandra_matmul_hardware_register16' in (bundle/'kernel0.metal').read_text(), 'compiler must include current register kernel'
post=(bundle/'kernel0-post.metal').read_text()
assert 'kalandra_matmul_fused_register16' in post, 'compiler must include current epilogue'
# Simulate an existing bundle: same standalone map, no optional fused entries.
legacy=a.output/'legacy';shutil.copytree(bundle,legacy,dirs_exist_ok=True)
plain=a.output/'legacy-post.metal';plain.write_text(post.split('\ninline float kalandra_fused_post',1)[0])
lock=json.loads((root/'native/metal-proof/toolchain.json').read_text())
metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',plain,'-o',a.output/'legacy-post.air'])
run([*metal,a.output/'legacy-post.air','-o',legacy/'kernel0-post.metallib'])
variants=[('optimized',['-O2'],'libkalandra-task-core-optimized.a'),('debug',['-O0'],'libkalandra-native-runtime.a'),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'],'libkalandra-native-runtime-sanitized.a'),('thread-sanitized',['-O1','-g','-fsanitize=thread','-fno-omit-frame-pointer'],'libkalandra-native-runtime-thread-sanitized.a')]
for name,flags,runtime in variants:
 for harness in ('PipelineGpu','RegisterMatmulGpu'):
  binary=a.output/(name+'-'+harness)
  run([clang,'-std=c++23','-ffp-contract=off','-pthread','-Wall','-Wextra','-Werror',*flags,'-isystem',llvm/'include',root/('native/tests/'+harness+'.cpp'),compiler.parent/'metal'/('libkalandra-metal-'+name+'.a'),compiler.parent/runtime,'-L'+str(llvm/'lib'),'-lLLVM','-Wl,-rpath,'+str(llvm/'lib'),'-framework','Foundation','-framework','Metal','-o',binary])
  print(name+': '+run([binary,bundle]).strip(),flush=True)
  if harness=='PipelineGpu':print(name+' legacy: '+run([binary,legacy,'--legacy']).strip(),flush=True)

# Source-level captures at the fusion/direct/argument-buffer boundaries.
for label,biases,scaled,fused in [('matrix-only',0,False,True),('two-scalars',1,True,True),('last-fused-direct',26,False,True),('staged-direct',27,False,False),('staged-indirect',29,False,False)]:
 tensor='Tensor[float32,?,?]'
 parameters=['a:'+tensor,'b:'+tensor]+[f'c{i}:'+tensor for i in range(biases)]+(['scale:float32','offset:float32'] if scaled else [])
 expression='matmul(a,b)'+''.join(f'+c{i}' for i in range(biases))
 if scaled:expression='('+expression+')*scale+offset'
 source='fn project('+','.join(parameters)+')->float32:\n  reduceSum(relu('+expression+'))\nfn main()->float32 ! TaskError:\n  try parallel:\n    a:Tensor[float32,2,3]=tensor([[1,1,1],[1,1,1]])\n    b:Tensor[float32,3,2]=tensor([[1,1],[1,1],[1,1]])\n    gpu=devices.gpu()\n    da=try await transfer(a,gpu)\n    db=try await transfer(b,gpu)\n'
 if biases:source+='    bias:Tensor[float32,2,2]=tensor([[1,1],[1,1]])\n    dc=try await transfer(bias,gpu)\n'
 args=['da','db']+['dc']*biases+(['2','1'] if scaled else [])
 source+='    try await spawn project('+','.join(args)+') on gpu\n'
 path=a.output/(label+'.k');path.write_text(source)
 expected=4*((3+biases)*2+1 if scaled else 3+biases)
 expected_bits=struct.unpack('I',struct.pack('f',expected))[0]
 for name,_,_ in variants:
  flag={'optimized':'-O2','debug':'-O0','sanitized':'--sanitize','thread-sanitized':'--thread-sanitize'}[name]
  target=a.output/(label+'-'+name)
  run([compiler,path,'--target','metal','--arithmetic','hardware',flag,'-o',target])
  assert ('kalandra_matmul_fused_tiled' in (target/'kernel0-post.metal').read_text())==fused,(label,name)
  assert int(run([target/'program',target]).strip(),16)==expected_bits,(label,name)
 print(label+': source captures, selected/fallback entry and all four modes passed',flush=True)
