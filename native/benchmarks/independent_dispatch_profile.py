import json,os,sys
from pathlib import Path
sys.path.insert(0,'native/benchmarks/fair');import protocol as p
b=(p.ROOT/'benchmarks/.build/gpu-independent-dispatch-v2-2026-10-29');bundle=p.ROOT/'benchmarks/.build/gpu-attention-2026-10-29/acceptance/bundle'
def invoke(cell,count,rounds):
 version,m,d=cell
 data=p.run([b/'profile',bundle,m,d,count,rounds,0,0,0,2,1],env=dict(os.environ,KALANDRA_QKV_CONCURRENT='1' if version=='current' else '0'))
 rows=p.observations(data,m*d*4,count,rounds)
 assert all(row['gpu_ns']>0 and row['encoding_ns']>0 for row in rows)
 return rows
result=p.measure_paired([(v,m,d) for m,d in [(1,512),(64,256),(16,512),(64,512)] for v in ('baseline','current')],invoke,lambda c:c[1]*c[2]*4,blocks=5,target_ms=50,adaptive=True,max_iterations=256)
result.update(acceptance=False,contract='Options.profile diagnostic: command GPU duration and CPU encoding time. No shader sampling. Numerical output qualification is separate; timed calls validate resource cleanup. Not performance acceptance.')
p.evidence(p.ROOT/'benchmarks/results/gpu-independent-dispatch-profile-2026-10-29.json',result,[Path(__file__),b/'Profile.cpp',b/'manifest.json'],[b/'profile',b/'metal.o',*bundle.glob('*.metallib')],p.ROOT/'build/native/native/kalandra')
