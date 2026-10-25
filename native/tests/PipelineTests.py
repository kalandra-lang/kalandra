"""Pure float32 projection/bias/ReLU pipeline numerical and lease conformance."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser();p.add_argument('--compiler',type=Path,required=True);p.add_argument('--clang',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[2];compiler=a.compiler.resolve();clang=a.clang.absolute();llvm=clang.parent.parent;a.output.mkdir(parents=True,exist_ok=True)
def run(args):
 r=subprocess.run(list(map(str,args)),text=True,capture_output=True,timeout=240)
 assert r.returncode==0,(args,r.returncode,r.stdout,r.stderr)
 return r.stdout
bundle=a.output/'bundle'
run([compiler,root/'native/benchmarks/Pipeline.k','--target','metal','--arithmetic','hardware','-O2','-o',bundle])
variants=[('optimized',['-O2'],'libkalandra-task-core-optimized.a'),('debug',['-O0'],'libkalandra-native-runtime.a'),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'],'libkalandra-native-runtime-sanitized.a'),('thread-sanitized',['-O1','-g','-fsanitize=thread','-fno-omit-frame-pointer'],'libkalandra-native-runtime-thread-sanitized.a')]
for name,flags,runtime in variants:
 binary=a.output/name
 run([clang,'-std=c++23','-ffp-contract=off','-pthread','-Wall','-Wextra','-Werror',*flags,'-isystem',llvm/'include',root/'native/tests/PipelineGpu.cpp',compiler.parent/'metal'/('libkalandra-metal-'+name+'.a'),compiler.parent/runtime,'-L'+str(llvm/'lib'),'-lLLVM','-Wl,-rpath,'+str(llvm/'lib'),'-framework','Foundation','-framework','Metal','-o',binary])
 print(name+': '+run([binary,bundle]).strip(),flush=True)
