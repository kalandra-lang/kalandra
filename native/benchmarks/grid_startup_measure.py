"""Warm-cache process startup plus three transformer invocations, not cold compilation."""
import argparse,json,statistics,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build',type=Path,required=True);parser.add_argument('--verification',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
a=parser.parse_args();assert not a.output.exists()
b=a.build.resolve();manifest=b/'manifest.json';frozen=json.loads(manifest.read_text())
q=json.loads(a.verification.read_text());assert len(q['checks'])==117 and all(r['bit_identical'] for r in q['checks'])
for field in ('signature','artifacts'):
 for name,h in frozen[field].items():assert p.sha(p.ROOT/name)==h,name
old=p.ROOT/'benchmarks/.build/gpu-closed-entry-2026-10-29/acceptance/gpu'
bundle=p.ROOT/'benchmarks/.build/gpu-attention-2026-10-29/acceptance/bundle'
records=[];pairs=[]
for block in range(7):
 for m in (16,64):
  rows=[]
  for v in (('baseline','current','current','baseline') if block%2==0 else ('current','baseline','baseline','current')):
   binary=old if v=='baseline' else b/'gpu';path=bundle if v=='baseline' else b/'bundle'
   start=time.perf_counter_ns()
   samples=p.observations(p.run([binary,path,m,512,1,1,0,0,0,2,1]),m*512*4,1,1)
   elapsed=time.perf_counter_ns()-start
   row=dict(block=block,m=m,version=v,process_ms=elapsed/1e6,invocation_ms=sum(s['ns'] for s in samples)/1e6,samples=samples)
   records.append(row);rows.append(row)
  base=statistics.median(r['process_ms'] for r in rows if r['version']=='baseline');new=statistics.median(r['process_ms'] for r in rows if r['version']=='current')
  pairs.append(dict(block=block,m=m,baseline_ms=base,current_ms=new,time_reduction_percent=(1-new/base)*100))
 print(block+1,'startup block complete',flush=True)
p.evidence(a.output,dict(contract=__doc__,scope='Process wall time includes loader, input preparation, three invocations and teardown. OS/Metal caches are already warm. It is not an isolated PSO compilation measure.',verification_sha256=p.sha(a.verification),records=records,pairs=pairs),[Path(__file__),manifest,a.verification],[old,*[p.ROOT/f for f in frozen['artifacts']]],p.ROOT/'build/native/native/kalandra')
