"""Qualify checkpointed device loops against independent integer oracles and CPU code.

No timing claim is made. A device test compiles and runs a real backend artifact;
compile-only is explicit for hosted hardware-less checks.
"""
import argparse,json,struct,subprocess,os
from pathlib import Path
from ScalarControlCases import cases, fault_cases
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--backend',choices=('metal','vulkan','cuda'),required=True)
p.add_argument('--root',type=Path,required=True)
p.add_argument('--compile-only',action='store_true')
p.add_argument('--variant',choices=('debug','optimized'),default='optimized')
a=p.parse_args();a.compiler=a.compiler.resolve();a.root.mkdir(parents=True,exist_ok=True)
flags=['-O0'] if a.variant=='debug' else ['-O2'];results=[]
suffix='.exe' if os.name=='nt' else ''
def run(cmd,env=None):
 r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=180,env=env)
 assert r.returncode==0,(cmd,r.returncode,r.stdout,r.stderr)
 return r.stdout.strip()
for name,(helper,call,oracle) in cases().items():
 d=a.root/name;d.mkdir(exist_ok=True)
 typ=helper.split('fn work(')[1].split('->',1)[1].split(':',1)[0]
 cpu=d/'cpu.k';cpu.write_text(helper+f'fn main()->{typ}:\n  {call}\n')
 run([a.compiler,cpu,*flags,'-o',d/('cpu'+suffix)])
 output=run([d/('cpu'+suffix)]);assert float(output)==oracle,(name,'CPU',output,oracle)
 gpu=d/'gpu.k';gpu.write_text(helper+f'fn main()->{typ} ! TaskError:\n  try parallel:\n    gpu=devices.gpu()\n    try await spawn {call} on gpu\n')
 bundle=d/'device';policy='hardware-with-software-fallback' if a.backend=='metal' and typ=='float64' else 'hardware'
 run([a.compiler,gpu,'--target',a.backend,'--arithmetic',policy,*flags,'-o',bundle])
 kind='DeviceControl' if name.startswith('application-error-') else 'ScalarControl'
 assert 'NumericInstructionKind::'+kind in (bundle/'host.cpp').read_text(),name
 # Do not mistake a host loop or an unrolled computation for device execution.
 shaders=list(bundle.glob('*.mlir'))
 assert any('scf.while' in f.read_text() and 'gpu.func' in f.read_text() for f in shaders),name
 if not a.compile_only:
  raw=run([bundle/('program'+suffix)],dict(os.environ,KALANDRA_WORKERS='1'));bits=int(raw,16)
  assert run([bundle/('program'+suffix)],dict(os.environ,KALANDRA_WORKERS='4'))==raw,name
  if typ.startswith('float'):
   width=32 if typ=='float32' else 64
   value=struct.unpack('<f' if width==32 else '<d',bits.to_bytes(width//8,'little'))[0]
  else:
   width=64 if typ=='int' else int(''.join(filter(str.isdigit,typ)))
   value=bits-(1<<width) if typ.startswith('int') and bits&(1<<(width-1)) else bits
  assert value==oracle,(name,'GPU',raw,value,oracle)
 results.append({'case':name,'expected':oracle,'status':'compiled' if a.compile_only else 'executed'})
 print(name,results[-1]['status'],flush=True)
# A scalar control result stays resident and feeds the ordinary tensor graph.
helper='fn steps(n:int)->float32:\n  total:float32=0\n  for i in range(n):\n    total=total+float32(i)\n  total\nfn graph(x:Tensor[float32,?],n:int)->float32:\n  reduceSum(x*steps(n))\n'
d=a.root/'tensor-helper-composition';d.mkdir(exist_ok=True)
cpu=d/'cpu.k';cpu.write_text(helper+'fn main()->float32:\n  graph(tensor([1,2,3]),10)\n')
run([a.compiler,cpu,*flags,'-o',d/('cpu'+suffix)])
assert float(run([d/('cpu'+suffix)]))==270
src=d/'gpu.k';src.write_text(helper+'fn main()->float32 ! TaskError:\n  try parallel:\n    gpu=devices.gpu()\n    x:Tensor[float32,3]=tensor([1,2,3])\n    resident=try await transfer(x,gpu)\n    try await spawn graph(resident,10) on gpu\n')
bundle=d/'device';run([a.compiler,src,'--target',a.backend,*flags,'-o',bundle])
assert 'ScalarControl' in (bundle/'host.cpp').read_text()
if not a.compile_only:
 assert run([bundle/('program'+suffix)])=='43870000'
results.append({'case':'tensor-helper-composition','expected':270,'status':'compiled' if a.compile_only else 'executed'})
print('tensor-helper-composition',results[-1]['status'],flush=True)
for name,(helper,call) in fault_cases().items():
 d=a.root/name;d.mkdir(exist_ok=True)
 cpu=d/'cpu.k';cpu.write_text(helper+'fn main()->int:\n  '+call+'\n')
 run([a.compiler,cpu,*flags,'-o',d/('cpu'+suffix)])
 reference=subprocess.run([str(d/('cpu'+suffix))],capture_output=True,text=True,timeout=30)
 assert reference.returncode!=0 and not reference.stdout,(name,reference)
 gpu=d/'gpu.k';gpu.write_text(helper+'fn main()->int ! TaskError:\n  try parallel:\n    gpu=devices.gpu()\n    try await spawn '+call+' on gpu\n')
 bundle=d/'device';run([a.compiler,gpu,'--target',a.backend,'--arithmetic','hardware',*flags,'-o',bundle])
 if not a.compile_only:
  result=subprocess.run([str(bundle/('program'+suffix))],capture_output=True,text=True,timeout=30)
  assert result.returncode!=0 and not result.stdout and '/4' in result.stderr,(name,result)
 results.append({'case':name,'status':'compiled' if a.compile_only else 'checked-fault'})
 print(name,results[-1]['status'],flush=True)
(a.root/'report.json').write_text(json.dumps({'backend':a.backend,'variant':a.variant,'gpuExecution':not a.compile_only,'results':results},indent=2)+'\n')
