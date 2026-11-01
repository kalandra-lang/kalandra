"""Public CUDA matrix-policy qualification with actual backend route evidence.

All output cells are validated; execution duration is not a performance result.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=True)
records=[]
for name,m,n,k in [('aligned',128,256,32),('row-tail',129,128,16),('contraction-tail',128,128,17)]:
    source=a.output/(name+'.k')
    left='['+','.join('['+','.join('1' for _ in range(k))+']' for _ in range(m))+']'
    right='['+','.join('['+','.join('1' for _ in range(n))+']' for _ in range(k))+']'
    source.write_text(f'''fn work(a:Tensor[float32,?,?],b:Tensor[float32,?,?])->Tensor[float32,?,?]:
  matmul(a,b)
fn main()->Tensor[float32,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    a:Tensor[float32,{m},{k}]=tensor({left})
    b:Tensor[float32,{k},{n}]=tensor({right})
    x=try await transfer(a,gpu)
    y=try await transfer(b,gpu)
    result=try await spawn work(x,y) on gpu
    try await transfer(result,devices.cpu())
''')
    expected=struct.unpack('<I',struct.pack('<f',k))[0]
    for policy in ('fixed-tree','optimized'):
      for arithmetic in ('hardware','software','hardware-with-software-fallback'):
        directory=a.output/(name+'-'+policy+'-'+arithmetic)
        built=subprocess.run([a.compiler,'--target','cuda','--matmul-policy',policy,'--arithmetic',arithmetic,
            '-O2',source,'-o',directory],capture_output=True,text=True,timeout=240)
        a.output.joinpath(directory.name+'.build.log').write_text(built.stdout+built.stderr)
        assert built.returncode==0,(directory,built.stderr[-4000:])
        notices=(directory/'THIRD_PARTY_NOTICES.txt').read_text()
        assert 'MIT License' in notices and 'Simon Boehm' in notices
        assert 'BSD-3-Clause' in notices and 'NVIDIA' in notices
        native=policy=='optimized' and arithmetic!='software' and name=='aligned'
        for workers in (1,2,4):
            run=subprocess.run([directory/'program'],env=os.environ|{'KALANDRA_WORKERS':str(workers),'KALANDRA_CUDA_DIAGNOSTICS':'1'},
                capture_output=True,text=True,timeout=90)
            values=[int(v,16) for v in run.stdout.strip().strip('[]').split(',') if v]
            record={'case':name,'policy':policy,'arithmetic':arithmetic,'workers':workers,'status':run.returncode,
                    'route':run.stderr,'cells':len(values),'stdoutSHA256':hashlib.sha256(run.stdout.encode()).hexdigest()}
            records.append(record);a.output.joinpath('results.json').write_text(json.dumps(records,indent=2)+'\n')
            assert not run.returncode and len(values)==m*n and all(v==expected for v in values),record
            assert f'route={"tiled" if native else "fixed-tree"}' in run.stderr,record
            assert f'tile={64 if native else 0}' in run.stderr and f'shape={m},{k},{n}' in run.stderr,record
        print('PASS:',name,policy,arithmetic,'all cells and actual route; workers1/2/4',flush=True)
print('PASS:',len(records),'public matrix executions; independent complete output, full-F32 policy, GPU tails and actual native/fixed route evidence')
