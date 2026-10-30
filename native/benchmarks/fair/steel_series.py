"""Run Steel acceptance studies sequentially with load recording from launch."""
import argparse,datetime,json,os,subprocess,sys
from pathlib import Path
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--build',type=Path,required=True)
ap.add_argument('--prefix',type=Path,required=True)
a=ap.parse_args()
assert (a.build/'qualification.json').exists()
script=Path(__file__).resolve().parents[1]/'mlx_steel_study.py'
for study in ('aa','screen','confirm','prototype','repeat'):
    output=a.prefix.with_name(a.prefix.name+'-'+study+'.json')
    load=output.with_name(output.stem+'-load.json')
    log=output.with_suffix('.log')
    assert not output.exists() and not load.exists() and not log.exists()
    command=[sys.executable,str(script),'--phase','measure','--build',str(a.build),
             '--output',str(output),'--study',study]
    if study in ('confirm','prototype','repeat'):
        command+=['--selection',str(a.prefix.with_name(a.prefix.name+'-screen.json'))]
    samples=[]
    with log.open('w') as stream:
        job=subprocess.Popen(command,stdout=stream,stderr=subprocess.STDOUT)
        while job.poll() is None:
            rows=[]
            for line in subprocess.check_output(['ps','-axo','pid=,ppid=,pcpu=,comm='],text=True).splitlines():
                pid,parent,cpu,exe=line.split(None,3)
                rows.append((int(pid),int(parent),float(cpu),exe))
            own={os.getpid(),job.pid}
            for _ in range(12):own|={pid for pid,parent,_,_ in rows if parent in own}
            external=[r for r in rows if r[0] not in own]
            samples.append(dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                external_cpu_percent=sum(r[2] for r in external),
                top=[dict(cpu_percent=r[2],exe=r[3]) for r in sorted(external,key=lambda r:r[2],reverse=True)[:8]]))
            load.write_text(json.dumps(dict(command=command,
                context='Isolated preallocated resident float32 kernels. No builds, correctness tests, profiling or other acceptance timings overlap. CPU load cannot establish GPU idleness. All samples retained.',
                samples=samples),indent=2)+'\n')
            try:job.wait(timeout=5)
            except subprocess.TimeoutExpired:pass
        assert job.returncode==0,log.read_text()[-3000:]
    print(study,'complete',flush=True)
