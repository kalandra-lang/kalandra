"""Sequential frozen/control/replication GPU acceptance; retain all raw evidence.

Configuration names ordinary{baseline,current} and cooperative{current,reference},
with [executable, shader] commands. Prepare/verify artifacts before running this.
No builds, tests, profiling, plotting or other acceptance may overlap this run.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--configuration',type=Path,required=True)
p.add_argument('--protocol',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--study',choices=('aa','frozen','repeat','cooperative-aa','reference','reference-repeat'),required=True)
a=p.parse_args();assert not a.output.exists()
cfg=json.loads(a.configuration.read_text())
spec=importlib.util.spec_from_file_location('protocol',a.protocol);protocol=importlib.util.module_from_spec(spec);spec.loader.exec_module(protocol)
sha=lambda path:hashlib.sha256(Path(path).read_bytes()).hexdigest()
artifacts={path:sha(path) for path in dict.fromkeys(x for command in cfg['commands'].values() for x in command)}
evidence=dict(study=a.study,configuration=cfg,configuration_sha256=sha(a.configuration),driver_protocol_sha256=sha(a.protocol),orchestrator_sha256=sha(__file__),artifacts=artifacts,utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),invocations=[],status='running')
def save():a.output.write_text(json.dumps(evidence,indent=2)+'\n')
previous_cpu={}
previous_time=None
last_cpu_sample=None
def load():
    global previous_cpu,previous_time,last_cpu_sample
    now=time.monotonic()
    if previous_time is not None and now-previous_time<.5:return last_cpu_sample
    states={}
    if cfg['platform']=='linux':
        for directory in Path('/proc').iterdir():
            if not directory.name.isdigit():continue
            try:
                fields=(directory/'stat').read_text();end=fields.rindex(')');parts=fields[end+2:].split()
                states[int(directory.name)]=(int(parts[1]),(int(parts[11])+int(parts[12]))/os.sysconf('SC_CLK_TCK'),fields[fields.index('(')+1:end])
            except (OSError,ValueError,IndexError):continue
    else:
        for line in subprocess.check_output(['ps','-axo','pid=,ppid=,time=,comm='],text=True).splitlines():
            pid,parent,cpu,exe=line.split(None,3)
            seconds=0
            for field in cpu.split(':'):seconds=seconds*60+float(field)
            states[int(pid)]=(int(parent),seconds,exe)
    own={os.getpid()}
    for _ in range(20):own|={pid for pid,(parent,_,_) in states.items() if parent in own}
    rows=[];elapsed=now-previous_time if previous_time is not None else 0
    for pid,(parent,seconds,exe) in states.items():
        if pid not in own and pid in previous_cpu and elapsed>0:
            rows.append((pid,parent,max(0,100*(seconds-previous_cpu[pid])/elapsed),exe))
    previous_cpu={pid:s[1] for pid,s in states.items()};previous_time=now
    last_cpu_sample=dict(utc=time.time(),total=sum(r[2] for r in rows),top=[dict(pid=r[0],cpu=r[2],exe=r[3]) for r in sorted(rows,key=lambda x:x[2],reverse=True)[:8]])
    return last_cpu_sample
class LoadRejected(AssertionError):pass
def require_idle(sample):
    if not admitted(sample):raise LoadRejected('External CPU load exceeded the fixed gate; evidence retained.')
def admitted(sample):return sample['total']<=200 and all(r['cpu']<=90 for r in sample['top'])
try:
    evidence['idle_samples']=[]
    for idle_attempt in range(5):
        passed=True
        for _ in range(3):
            sample=load();sample['idle_attempt']=idle_attempt;evidence['idle_samples'].append(sample);save()
            passed=passed and admitted(sample)
            if cfg['platform']=='linux':
                gpu=subprocess.check_output(['nvidia-smi','--query-gpu=utilization.gpu','--format=csv,noheader,nounits'],text=True).strip()
                evidence.setdefault('gpu_idle_samples',[]).append(gpu);save()
                passed=passed and all(int(n.strip())<=2 for n in gpu.splitlines())
            time.sleep(1)
        if passed:break
        if idle_attempt==4:raise LoadRejected('Initial CPU/GPU idle gate failed after five retained windows.')
        time.sleep(2)
    def invoke_once(cell,count,rounds):
        version,n=cell
        if a.study=='aa':key='ordinary-current'
        elif a.study=='cooperative-aa':key='cooperative-current'
        elif a.study in ('frozen','repeat'):key='ordinary-'+('baseline' if version=='baseline' else 'current')
        else:key='cooperative-'+('reference' if version=='baseline' else 'current')
        command=[*cfg.get('prefix',[]),*cfg['commands'][key],str(n),str(count),str(rounds)]
        record=dict(cell=cell,iterations=count,rounds=rounds,command=command,load=[load()]);evidence['invocations'].append(record);save()
        require_idle(record['load'][0])
        with subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True) as job:
            while job.poll() is None:
                time.sleep(.5)
                record['load'].append(load())
            stdout,stderr=job.communicate()
        record.update(returncode=job.returncode,stdout=stdout,stderr=stderr);save()
        assert job.returncode==0 and not stderr,(command,stdout,stderr)
        for sample in record['load']:require_idle(sample)
        return protocol.observations(stdout,float(n*4),count,rounds)
    def invoke(cell,count,rounds):
        for attempt in range(5):
            try:return invoke_once(cell,count,rounds)
            except LoadRejected as error:
                evidence['invocations'][-1]['load_rejected']=str(error);save()
                if attempt==4:raise
                time.sleep(2)
    cells=[(version,n) for n in (16,65537,1048576) for version in ('baseline','current')]
    evidence['measurement']=protocol.measure_paired(cells,invoke,lambda c:float(c[1]*4),blocks=3,rounds=5,target_ms=100,adaptive=True,max_iterations=3000)
    if a.study in ('aa','cooperative-aa'):
        checks=[]
        import statistics
        for group in {tuple(pair['group']) for pair in evidence['measurement']['pairs']}:
            deltas=[1/pair['speedup']-1 for pair in evidence['measurement']['pairs'] if tuple(pair['group'])==group]
            checks.append(dict(group=group,median_change=statistics.median(deltas),worst_change=max(map(abs,deltas)),passed=abs(statistics.median(deltas))<=.05 and max(map(abs,deltas))<=.10))
        evidence['control_checks']=checks;save();assert all(c['passed'] for c in checks),'Identical-build control exceeded 5% median / 10% worst paired-block limits; retained.'
    evidence['status']='passed';save()
except BaseException as error:
    evidence.update(status='failed',error=str(error));save();raise
print(a.study,'passed',flush=True)
