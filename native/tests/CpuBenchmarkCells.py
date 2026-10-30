"""Check every output cell of the actual fair benchmark matrices, independently."""
import argparse,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--compiler',type=Path,required=True);p.add_argument('--clang',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
def run(args):
 r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=180)
 assert r.returncode==0,(args,r.returncode,r.stdout,r.stderr)
 return r.stdout
for dtype in ('float32','float64'):
 source=a.output/(dtype+'.k');source.write_text(f'fn cells(a:Tensor[{dtype},?,?],b:Tensor[{dtype},?,?])->Tensor[{dtype},?,?]:\n  matmul(a,b)\nfn main()->{dtype}:\n  0\n')
 ir=source.with_suffix('.ll');binary=source.with_suffix('')
 run([a.compiler,source,'--arithmetic','hardware','--test-entry','cells','--emit-llvm',ir,'-O2','-o',str(binary)+'-standalone'])
 run([a.clang,'-std=c++23','-O2','-ffp-contract=off','-pthread','-Wno-override-module',*(['-DFLOAT32'] if dtype=='float32' else []),*(['-fms-runtime-lib=static'] if sys.platform=='win32' else []),root/'native/tests/CpuBenchmarkCells.cpp',ir,a.compiler.parent/('kalandra-native-runtime.lib' if sys.platform=='win32' else 'libkalandra-native-runtime.a'),'-o',binary])
 for n in (256,512,1024):
  print(dtype+': '+run([binary,n]).strip(),flush=True)
