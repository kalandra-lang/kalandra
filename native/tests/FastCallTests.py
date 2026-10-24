"""Check generated call fast paths with runtime faults, migration and small stacks."""
import argparse,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--compiler',type=Path,required=True);p.add_argument('--clang',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
source=a.output/'fast.k'
source.write_text('''fn tree(x:int,depth:int)->int:
  if depth == 0:
    x
  else:
    tree(x*2,depth-1)+tree(x*2+1,depth-1)
fn main()->int:
  tree(17,5)
''')
def run(args):
 r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=180)
 assert r.returncode==0,(args,r.returncode,r.stdout,r.stderr)
 return r.stdout
variants=[('debug','-O0',['-O0']),('optimized','-O2',['-O2']),('sanitized','--sanitize',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'])]
if sys.platform!='win32':variants.append(('thread-sanitized','--thread-sanitize',['-O1','-g','-fsanitize=thread','-fno-omit-frame-pointer']))
for name,flag,flags in variants:
 binary=a.output/name;ir=binary.with_suffix('.ll')
 run([a.compiler,source,'--test-entry','tree','--emit-llvm',ir,flag,'-o',str(binary)+'-standalone'])
 text=ir.read_text()
 assert 'kalandra_source_run_v3' in text
 assert 'llvm.read_volatile_register' in text and 'tail.unwind' not in text
 runtime=a.compiler.parent/(('' if sys.platform=='win32' else 'lib')+'kalandra-native-runtime'+('-'+name if 'sanitized' in name else '')+('.lib' if sys.platform=='win32' else '.a'))
 run([a.clang,'-std=c++23','-Wno-override-module','-pthread',*flags,*(['-fms-runtime-lib=static'] if sys.platform=='win32' else []),root/'native/tests/FastCallRuntimeTests.cpp',ir,runtime,'-o',binary])
 print(name+': '+run([binary]).strip(),flush=True)
