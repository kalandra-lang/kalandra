"""Complete-source ten-type matrix/reduction correctness on a physical CUDA GPU.

Independent integer references are exactly representable in both floating types.
No timings, vendor-library substitution or installation.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--execute',action='store_true',required=True)
a=p.parse_args();a.compiler=a.compiler.resolve();a.output=a.output.resolve()
a.output.mkdir(parents=True,exist_ok=False)
records=[]
left=[[1,2,3],[0,2,1]];right=[[2,1],[1,3],[0,2]]
product=[sum(left[i][t]*right[t][j] for t in range(3)) for i in range(2) for j in range(2)]
for numeric in ('int8','uint8','int16','uint16','int32','uint32','int64','uint64','float32','float64'):
  width=32 if numeric=='float32' else 64 if numeric=='float64' else int(numeric.removeprefix('u').removeprefix('int'))
  def bits(value):
    return int.from_bytes(struct.pack('<f' if width==32 else '<d',value),'little') if numeric.startswith('float') else value
  for operation in ('matrix','reduction','reduction-tail'):
    if operation=='matrix':
      source=f'''fn work(a:Tensor[{numeric},?,?],b:Tensor[{numeric},?,?])->Tensor[{numeric},?,?]:
  matmul(a,b)
fn main()->Tensor[{numeric},?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    a:Tensor[{numeric},2,3]=tensor({left})
    b:Tensor[{numeric},3,2]=tensor({right})
    x=try await transfer(a,gpu)
    y=try await transfer(b,gpu)
    result=try await spawn work(x,y) on gpu
    try await transfer(result,devices.cpu())
'''
      expected='['+','.join(f'{bits(v):0{width//4}x}' for v in product)+']\n'
    else:
      values=list(range(1,6)) if operation=='reduction' else [0]*256+[5]
      source=f'''fn work(a:Tensor[{numeric},?])->{numeric}:
  reduceSum(a)
fn main()->{numeric} ! TaskError:
  try parallel:
    gpu=devices.gpu()
    a:Tensor[{numeric},{len(values)}]=tensor({values})
    x=try await transfer(a,gpu)
    try await spawn work(x) on gpu
'''
      expected=f'{bits(sum(values)):0{width//4}x}\n'
    path=a.output/(numeric+'-'+operation+'.k');path.write_text(source)
    for policy in ('hardware','software','hardware-with-software-fallback'):
      directory=a.output/(path.stem+'-'+policy)
      built=subprocess.run([a.compiler,'--target','cuda',path,'--arithmetic',policy,'-O2','-o',directory],capture_output=True,text=True,timeout=240)
      (a.output/(directory.name+'.build.log')).write_text(built.stdout+built.stderr)
      assert not built.returncode,(directory,built.stderr[-4000:])
      for workers in (1,2,4):
        result=subprocess.run([directory/'program'],env=os.environ|{'KALANDRA_WORKERS':str(workers)},capture_output=True,text=True,timeout=90)
        record={'type':numeric,'operation':operation,'policy':policy,'workers':workers,'status':result.returncode,'stdout':result.stdout,'stderr':result.stderr,'expected':expected}
        records.append(record);(a.output/'results.json').write_text(json.dumps(records,indent=2)+'\n')
        assert not result.returncode and not result.stderr and result.stdout==expected,record
      print('PASS',numeric,operation,policy,'workers1/2/4',flush=True)
print('PASS:',len(records),'complete-source numerical executions; ten types, full matrix output and odd/tail reductions')
