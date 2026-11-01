"""Frozen production-resource resident comparison. Requires an explicitly idle GPU."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import time
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--binary',type=Path,required=True)
p.add_argument('--catalog',type=Path,required=True)
p.add_argument('--baseline',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
paths={k:getattr(a,k).resolve() for k in ('binary','catalog','baseline')}
out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
def hashes():return {k:hashlib.sha256(v.read_bytes()).hexdigest() for k,v in paths.items()}
metadata={'scope':'production PTX, prepared resident launches; excludes allocation/transfers/tasks/JIT',
          'hashes':hashes(),'controlTolerance':0.05,'idleThresholdPercent':2,'preflight':[]}
for _ in range(3):
    sample=subprocess.check_output(['nvidia-smi','--query-gpu=timestamp,name,utilization.gpu,clocks.current.sm,temperature.gpu,memory.used','--format=csv,noheader'],text=True).strip()
    metadata['preflight'].append(sample)
    if any(int(line.split(',')[2].strip().split()[0])>2 for line in sample.splitlines()):
        (out/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
        raise SystemExit('GPU not idle; no timing started')
    time.sleep(.5)
(out/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
with (out/'gpu.csv').open('w') as log:
    monitor=subprocess.Popen(['nvidia-smi','--query-gpu=timestamp,utilization.gpu,clocks.current.sm,temperature.gpu,power.draw,memory.used','--format=csv,noheader','-l','1'],stdout=log,stderr=subprocess.STDOUT)
    try:
        with (out/'run.log').open('w') as output:
            result=subprocess.run([paths['binary'],paths['catalog'],out/'raw.jsonl',paths['baseline']],stdout=output,stderr=subprocess.STDOUT,timeout=600)
    finally:
        monitor.terminate();monitor.wait(timeout=10)
assert result.returncode==0,(out/'run.log').read_text()
assert hashes()==metadata['hashes'],'Inputs changed during measurement'
raw=[json.loads(line) for line in (out/'raw.jsonl').read_text().splitlines()]
assert len(raw)==1537, len(raw)
groups={};controls=[]
for start in range(1,len(raw),4):
    block=raw[start:start+4]
    assert [r['position'] for r in block]==[0,1,2,3]
    assert [r['side'] for r in block]==[0,1,1,0]
    key=tuple(block[0][n] for n in ('m','n','k','a','b'))
    assert all(tuple(r[n] for n in ('m','n','k','a','b'))==key for r in block)
    ratio=statistics.median(r['gpuUs'] for r in block if r['side']==1)/statistics.median(r['gpuUs'] for r in block if r['side']==0)
    groups.setdefault(key,[]).append(ratio)
    if key[3]==key[4]:controls.append({'shape':key[:3],'candidate':key[3],'ratio':ratio,'accepted':abs(ratio-1)<=.05})
assert len(controls)==192
report={'allControlsPass':all(c['accepted'] for c in controls),'controls':controls,
        'comparisons':[{'shape':key[:3],'a':key[3],'b':key[4],'bOverA':statistics.median(values),'blockRatios':values} for key,values in groups.items()]}
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS correctness; 192 stability controls:',report['allControlsPass'],flush=True)
if not report['allControlsPass']:raise SystemExit('Stability controls failed; all samples retained, no acceptance claim')
