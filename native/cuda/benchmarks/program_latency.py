"""Whole generated-application latency, distinct from resident kernel throughput.

Preparation and timing are separate invocations. Execution includes process and
private CUDA-context setup, module loading/JIT cache, transfers, scheduling,
computation, result formatting and cleanup. It excludes compilation. Driver disk
cache is left untouched, so a first observation is not claimed to be a cold JIT.
"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import struct
import subprocess
import time
p=argparse.ArgumentParser(description=__doc__)
mode=p.add_mutually_exclusive_group(required=True)
mode.add_argument('--prepare',action='store_true');mode.add_argument('--execute',action='store_true')
p.add_argument('--compiler',type=Path)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output=a.output.resolve()
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def inventory(directory):return {str(p.relative_to(directory)):sha(p) for p in sorted(directory.rglob('*')) if p.is_file()}
if a.prepare:
    assert a.compiler,'--prepare needs --compiler'
    a.output.mkdir(parents=True,exist_ok=False)
    metadata={'scope':__doc__,'compilerSHA256':sha(a.compiler),'controlTolerance':.05,'cases':[]}
    for size in (64,256):
        row='['+','.join('1' for _ in range(size))+']'
        matrix='['+','.join(row for _ in range(size))+']'
        source=a.output/f'matrix-{size}.k'
        source.write_text(f'''fn work(a:Tensor[float32,?,?],b:Tensor[float32,?,?])->Tensor[float32,?,?]:
  matmul(a,b)
fn main()->Tensor[float32,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    a:Tensor[float32,{size},{size}]=tensor({matrix})
    b:Tensor[float32,{size},{size}]=tensor({matrix})
    x=try await transfer(a,gpu)
    y=try await transfer(b,gpu)
    result=try await spawn work(x,y) on gpu
    try await transfer(result,devices.cpu())
''')
        for policy in ('fixed-tree','optimized'):
            directory=a.output/f'{size}-{policy}'
            with (a.output/f'{size}-{policy}.build.log').open('w') as log:
                subprocess.run([a.compiler,'--target','cuda','--matmul-policy',policy,source,'-O2','-o',directory],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=300)
            metadata['cases'].append({'size':size,'policy':policy,'directory':directory.name,'hashes':inventory(directory)})
    (a.output/'frozen.json').write_text(json.dumps(metadata,indent=2)+'\n')
    print('Prepared and hashed whole-application benchmarks; no timings collected')
else:
    assert not a.compiler,'timing must not compile or alter the frozen programs'
    metadata=json.loads((a.output/'frozen.json').read_text());raw=a.output/'process-samples.jsonl'
    assert not raw.exists(),'never overwrite an earlier series'
    def gpu():return subprocess.check_output(['nvidia-smi','--query-gpu=timestamp,name,utilization.gpu,clocks.current.sm,temperature.gpu,memory.used','--format=csv,noheader'],text=True).strip()
    preflight=[]
    for _ in range(3):
        sample=gpu();preflight.append(sample)
        assert all(int(line.split(',')[2].strip().split()[0])<=2 for line in sample.splitlines()),'competing GPU load; no series started'
        time.sleep(.5)
    (a.output/'process-preflight.json').write_text(json.dumps(preflight,indent=2)+'\n')
    cases={(c['size'],c['policy']):c for c in metadata['cases']}
    for case in cases.values():assert inventory(a.output/case['directory'])==case['hashes'],'frozen input changed'
    records=[]
    def invoke(size,policy,phase,block,side):
        binary=a.output/cases[size,policy]['directory']/'program'
        start=time.perf_counter_ns();result=subprocess.run([binary],capture_output=True,timeout=90);elapsed=(time.perf_counter_ns()-start)/1e6
        values=[int(v,16) for v in result.stdout.strip().strip(b'[]').split(b',') if v]
        expected=int.from_bytes(struct.pack('<f',size),'little')
        record={'size':size,'policy':policy,'phase':phase,'block':block,'side':side,'wallMs':elapsed,'status':result.returncode,'cells':len(values),'stdoutSHA256':hashlib.sha256(result.stdout).hexdigest()}
        with raw.open('a') as log:log.write(json.dumps(record)+'\n')
        records.append(record)
        assert not result.returncode and not result.stderr and len(values)==size*size and all(v==expected for v in values),(record,result.stderr)
    with (a.output/'process-gpu.csv').open('w') as log:
        monitor=subprocess.Popen(['nvidia-smi','--query-gpu=timestamp,index,utilization.gpu,clocks.current.sm,temperature.gpu,memory.used','--format=csv,noheader','-l','1'],stdout=log,stderr=subprocess.STDOUT)
        try:
            for size in (64,256):
                for policy in ('fixed-tree','optimized'):invoke(size,policy,'first-observation',-1,0)
                for phase,policies in [('identical-control',('fixed-tree','fixed-tree')),('comparison',('fixed-tree','optimized'))]:
                    for block in range(4):
                        for side in (0,1,1,0):invoke(size,policies[side],phase,block,side)
        finally:
            monitor.terminate();monitor.wait(timeout=10)
    for case in cases.values():assert inventory(a.output/case['directory'])==case['hashes'],'frozen input changed during series'
    controls=[];comparisons=[]
    for size in (64,256):
        for phase in ('identical-control','comparison'):
            ratios=[]
            for block in range(4):
                rows=[r for r in records if (r['size'],r['phase'],r['block'])==(size,phase,block)]
                ratio=statistics.median(r['wallMs'] for r in rows if r['side']==1)/statistics.median(r['wallMs'] for r in rows if r['side']==0)
                ratios.append(ratio)
                if phase=='identical-control':controls.append({'size':size,'block':block,'ratio':ratio,'accepted':abs(ratio-1)<=metadata['controlTolerance']})
            if phase=='comparison':comparisons.append({'size':size,'optimizedOverFixed':statistics.median(ratios),'blockRatios':ratios})
    report={'scope':__doc__,'allControlsPass':all(c['accepted'] for c in controls),'controls':controls,'comparisons':comparisons,'postflight':gpu()}
    (a.output/'process-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    if not report['allControlsPass']:raise SystemExit('Control gate failed; samples retained, no performance acceptance claim')
