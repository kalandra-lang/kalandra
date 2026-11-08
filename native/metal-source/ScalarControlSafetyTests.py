"""Physical checkpoint lifetime/cancellation tests using generated Metal code."""
import argparse,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--clang',type=Path,required=True)
p.add_argument('--metal-runtime',type=Path,required=True)
p.add_argument('--runtime',type=Path,required=True)
p.add_argument('--variant',choices=('optimized','sanitized','thread-sanitized'),required=True)
p.add_argument('--root',type=Path,required=True)
a=p.parse_args();repo=Path(__file__).resolve().parents[2];a.root=a.root.resolve();a.root.mkdir(parents=True,exist_ok=True)
flags={'optimized':['-O2'],'sanitized':['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'],'thread-sanitized':['-O1','-g','-fsanitize=thread','-fno-omit-frame-pointer']}[a.variant]
def run(cmd):
 r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=180)
 assert not r.returncode,(cmd,r.returncode,r.stdout,r.stderr)
 assert not any(x in r.stderr for x in ('AddressSanitizer','ThreadSanitizer','runtime error:')),r.stderr
 return r.stdout.strip()
sources={
 'finite':'type Step:\n  value:i64\n  enabled:bool\nfn work(n:i64)->i64:\n  i=0\n  total=0\n  while i<n:\n    i=i+1\n    step=Step {value:i,enabled:true}\n    if step.enabled:\n      total=total+step.value\n  return total\n',
 'infinite':'fn work(n:i64)->i64:\n  i=0\n  return while true:\n    i=i+1\n',
 'fault':'fn work(n:i64)->i64:\n  i=0\n  total=0\n  while i<n:\n    total=total+1/(n-n)\n    i=i+1\n  return total\n'}
for name,helper in sources.items():
 src=a.root/(name+'.k');src.write_text(helper+'fn main()->i64 ! TaskError:\n  return try parallel:\n    gpu=devices.gpu()\n    try await spawn work(1000) on gpu\n')
 fixture=a.root/name
 run([a.compiler.resolve(),src,'--target','metal','-O2','-o',fixture])
 binary=a.root/(name+'-probe')
 run([a.clang,'-std=c++23','-Wall','-Wextra','-Werror','-ffp-contract=off','-I',repo/'native/metal',*flags,
      '-DKALANDRA_CONTROL_HOST="'+str(fixture/'host.cpp')+'"',repo/'native/metal-source/ScalarControlRuntimeTests.cpp',
      a.metal_runtime.resolve(),a.runtime.resolve(),'-framework','Foundation','-framework','Metal','-o',binary])
 mode=[] if name=='finite' else ['fault' if name=='fault' else 'cancel-only']
 print(a.variant,name,run([binary,fixture,*mode]),flush=True)
