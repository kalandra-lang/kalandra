"""Sequential frozen paired library measurements; retain failures and GPU load.

Only the explicit --acceptance phase records performance. It never builds.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--smoke',action='store_true')
p.add_argument('--acceptance',action='store_true')
p.add_argument('--gpu',choices=('nvidia','intel'),default='nvidia')
p.add_argument('--threads',type=int,default=16)
a=p.parse_args()
if a.smoke==a.acceptance:p.error('Choose --smoke or --acceptance')
a.build=a.build.resolve();a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=False)
manifest=json.loads((a.build/'manifest.json').read_text())
def frozen():
    for name,expected in manifest['hashes'].items():
        with Path(name).open('rb') as stream:assert hashlib.file_digest(stream,'sha256').hexdigest()==expected,name
frozen()
env=os.environ|{'VK_DRIVER_FILES':'/usr/share/vulkan/icd.d/'+a.gpu+'_icd.x86_64.json',
                'OPENBLAS_NUM_THREADS':'1','OMP_NUM_THREADS':str(a.threads),'OMP_DYNAMIC':'FALSE'}
def cpu_snapshot():
    values=list(map(int,Path('/proc/stat').read_text().splitlines()[0].split()[1:]))
    return sum(values),values[3]+values[4]
def quiet():
    before=cpu_snapshot();time.sleep(.5);after=cpu_snapshot()
    busy=1-(after[1]-before[1])/max(1,after[0]-before[0])
    gpu=subprocess.check_output(['nvidia-smi','--query-gpu=utilization.gpu,memory.used','--format=csv,noheader,nounits'],text=True).strip()
    result={'cpuBusyFraction':busy,'nvidiaUtilizationAndMemory':gpu}
    return result
records=[]
def trial(label,engine,n,pattern,policy,threads,rounds):
    before=quiet()
    command=[manifest['binary'],engine,str(n),str(pattern),str(threads),str(a.build/policy),manifest['blas'],str(rounds)]
    started=time.time()
    result=subprocess.run(command,env=env,capture_output=True,text=True,timeout=600)
    after=quiet()
    raw={'label':label,'command':command,'startUnix':started,'status':result.returncode,
         'stdout':result.stdout,'stderr':result.stderr,'before':before,'after':after}
    (a.output/(label+'.json')).write_text(json.dumps(raw,indent=2)+'\n')
    assert not result.returncode,(label,result.stderr)
    lines=[json.loads(line) for line in result.stdout.splitlines()]
    assert lines[0]['kind']=='initialization' and lines[-1]['kind']=='correctness'
    assert lines[-1]['immutable_inputs'] and lines[-1]['resources_retired']
    samples=[line['ns']/line['iterations'] for line in lines if line['kind']=='sample']
    assert len(samples)==rounds
    record={'label':label,'engine':engine,'n':n,'pattern':pattern,'policy':policy,'threads':threads,
            'nsPerCall':statistics.median(samples),'samplesNsPerCall':samples,
            'initialization':lines[0],'correctness':lines[-1],'before':before,'after':after}
    records.append(record)
    (a.output/'results.json').write_text(json.dumps({'manifest':manifest,'phase':'smoke' if a.smoke else 'acceptance','gpu':a.gpu,'records':records},indent=2)+'\n')
    print(label,record['nsPerCall'],'ns/call',flush=True)
    return record
if a.smoke:
    for n in (8,33):
        for pattern in (0,1):
            for engine,policy,threads in [('cpu','fixed-tree',1),('blas','fixed-tree',1),('blas','fixed-tree',a.threads),
                 ('gpu-resident','fixed-tree',1),('gpu-resident','optimized',1),('gpu-transfers','optimized',1)]:
                trial(f'{engine}-{policy}-{threads}-{n}-{pattern}',engine,n,pattern,policy,threads,1)
else:
    for n in (128,512,1024):
        for pattern in (0,1):
            # Frozen A/A controls before drawing comparative conclusions.
            controls=[]
            for block in range(3):
                order=('A','B','B','A') if block%2==0 else ('B','A','A','B')
                for position,tag in enumerate(order):
                    controls.append((tag,trial(f'control-{n}-{pattern}-{block}-{position}-{tag}',
                        'gpu-resident',n,pattern,'optimized',1,3)))
            medians={tag:statistics.median(row['nsPerCall'] for label,row in controls if label==tag) for tag in ('A','B')}
            ratio=medians['A']/medians['B']
            (a.output/f'control-{n}-{pattern}.json').write_text(json.dumps({'median':medians,'ratio':ratio,'accepted':.9<=ratio<=1.1},indent=2)+'\n')
            assert .9<=ratio<=1.1,('Unstable identical-build control; retain this rejection',n,pattern,ratio)
            cases=[('cpu','fixed-tree',1),('blas','fixed-tree',1),('blas','fixed-tree',a.threads),
                   ('gpu-resident','fixed-tree',1),('gpu-resident','optimized',1),('gpu-transfers','optimized',1)]
            for block in range(3):
                order=cases if block%2==0 else list(reversed(cases))
                for index,(engine,policy,threads) in enumerate(order):
                    trial(f'comparison-{n}-{pattern}-{block}-{index}-{engine}-{policy}-{threads}',engine,n,pattern,policy,threads,5)
frozen()
print('PASS: frozen sequential measurements and independent full-output checks; inspect controls/load before acceptance',flush=True)
