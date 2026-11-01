"""Sequential frozen CUDA/cuBLAS kernel reference with explicit control gates.

Not a language/end-to-end benchmark. Does not build, install, reset or change clocks.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import time
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--binary',type=Path,required=True)
p.add_argument('--ptx',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--execute',action='store_true',required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def gpu():return subprocess.check_output(['nvidia-smi','--query-gpu=timestamp,name,driver_version,utilization.gpu,clocks.current.sm,temperature.gpu,memory.used','--format=csv,noheader'],text=True).strip()
metadata={'binarySHA256':sha(a.binary),'ptxSHA256':sha(a.ptx),'controlTolerance':.05,'scope':'resident production-descriptor entrypoints; excludes allocation/transfers/tasks/compilation','preflight':[]}
for tile in (64,128):
    wrapped=a.ptx.with_name(f'matmul-warp{tile}.ptx')
    assert wrapped.read_bytes().split(b'// PTX\n',1)[1]==a.ptx.read_bytes(),'benchmark payload differs from production resource'
    metadata[f'productionWrapped{tile}SHA256']=sha(wrapped)
for _ in range(3):
    metadata['preflight'].append(gpu());time.sleep(.5)
    if any(int(line.split(',')[3].strip().split()[0])>2 for line in metadata['preflight'][-1].splitlines()):
        (a.output/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
        raise SystemExit('GPU background load exceeds predeclared 2%; no timings started')
(a.output/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
with (a.output/'gpu.csv').open('w') as log:
    monitor=subprocess.Popen(['nvidia-smi','--query-gpu=timestamp,index,utilization.gpu,clocks.current.sm,temperature.gpu,power.draw,memory.used','--format=csv,noheader','-l','1'],stdout=log,stderr=subprocess.STDOUT)
    try:
        with (a.output/'run.log').open('w') as output:
            result=subprocess.run([a.binary,a.ptx,a.output/'raw.jsonl'],stdout=output,stderr=subprocess.STDOUT,timeout=180)
    finally:monitor.terminate();monitor.wait(timeout=10)
assert result.returncode==0,(a.output/'run.log').read_text()
rows=[json.loads(line) for line in (a.output/'raw.jsonl').read_text().splitlines()]
metadata['benchmark']=rows[0];metadata['postflight']=gpu()
assert sha(a.binary)==metadata['binarySHA256'] and sha(a.ptx)==metadata['ptxSHA256'],'frozen input changed'
groups=defaultdict(lambda:defaultdict(list))
for row in rows[1:]:groups[(row['m'],row['n'],row['k'],row['a'],row['b'],row['block'])][row['side']].append(row['gpuUs'])
controls=[];comparisons=defaultdict(list)
for (m,n,k,x,y,block),sides in groups.items():
    ratio=statistics.median(sides[1])/statistics.median(sides[0])
    if x==y:controls.append({'m':m,'n':n,'k':k,'block':block,'ratio':ratio,'accepted':abs(ratio-1)<=.05})
    comparisons[(m,n,k,x,y)].append(ratio)
report={'metadata':metadata,'allControlsPass':all(c['accepted'] for c in controls),'controls':controls,
        'comparisons':[{'m':key[0],'n':key[1],'k':key[2],'a':key[3],'b':key[4],'bOverA':statistics.median(values),'blockRatios':values} for key,values in comparisons.items()]}
(a.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'allControlsPass':report['allControlsPass'],'comparisons':report['comparisons']},indent=2))
if not report['allControlsPass']:raise SystemExit('Control gate failed; all samples retained, no acceptance claim')
