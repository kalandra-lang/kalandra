"""Sequential full-model acceptance with complete load records and frozen inputs."""
import argparse,datetime,hashlib,json,os,statistics,subprocess,sys
from pathlib import Path
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--build',type=Path,required=True)
ap.add_argument('--prefix',type=Path,required=True)
ap.add_argument('--baseline',type=Path)
ap.add_argument('--baseline-bundle',type=Path)
a=ap.parse_args();assert (a.build/'qualification.json').exists()
orchestrator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
script=Path(__file__).resolve().parents[1]/'steel_integration.py'
for study in ('aa','frozen','policy','repeat','mps'):
 output=a.prefix.with_name(a.prefix.name+'-'+study+'.json')
 load=output.with_name(output.stem+'-load.json');log=output.with_suffix('.log')
 assert not output.exists() and not load.exists() and not log.exists()
 command=[sys.executable,str(script),'--phase','measure','--build',str(a.build),'--output',str(output),'--study',study]
 for name in ('baseline','baseline_bundle'):
  value=getattr(a,name)
  if value is not None:command += ['--'+name.replace('_','-'),str(value)]
 samples=[]
 with log.open('w') as stream:
  job=subprocess.Popen(command,stdout=stream,stderr=subprocess.STDOUT)
  while job.poll() is None:
   rows=[]
   for line in subprocess.check_output(['ps','-axo','pid=,ppid=,pcpu=,comm='],text=True).splitlines():
    pid,parent,cpu,exe=line.split(None,3);rows.append((int(pid),int(parent),float(cpu),exe))
   own={os.getpid(),job.pid}
   for _ in range(12):own|={pid for pid,parent,_,_ in rows if parent in own}
   external=[r for r in rows if r[0] not in own]
   samples.append(dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),external_cpu_percent=sum(r[2] for r in external),
    top=[dict(cpu_percent=r[2],exe=r[3]) for r in sorted(external,key=lambda r:r[2],reverse=True)[:8]]))
   load.write_text(json.dumps(dict(command=command,orchestrator_sha256=orchestrator_sha256,context='Full transformer acceptance, resident and owned-upload guards. No builds/tests/profiling or overlapping acceptance. CPU load does not prove GPU idleness. Every sample retained.',samples=samples),indent=2)+'\n')
   try:job.wait(timeout=5)
   except subprocess.TimeoutExpired:pass
  assert job.returncode==0,log.read_text()[-4000:]
 if study=='aa':
  data=json.loads(output.read_text());checks=[]
  for group in {tuple(r['group']) for r in data['pairs']}:
   changes=[1/r['speedup']-1 for r in data['pairs'] if tuple(r['group'])==group]
   checks.append(dict(group=group,median_time_change=statistics.median(changes),worst_absolute_time_change=max(map(abs,changes)),
                      passed=abs(statistics.median(changes))<=.03 and max(map(abs,changes))<=.06))
  control=output.with_name(output.stem+'-control.json');assert not control.exists()
  control.write_text(json.dumps(dict(limits=dict(median=.03,worst=.06),checks=checks),indent=2)+'\n')
  assert all(r['passed'] for r in checks),'AA control exceeded limits; results retained, comparisons not started.'
 print(study,'complete',flush=True)
