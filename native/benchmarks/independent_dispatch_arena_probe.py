"""Diagnostic GPU/CPU timing control for concurrent groups with separate parameters."""
import os,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'));import protocol as p
b=p.ROOT/'benchmarks/.build/gpu-independent-dispatch-v2-2026-10-29';bundle=p.ROOT/'benchmarks/.build/gpu-attention-2026-10-29/acceptance/bundle'
def invoke(cell,count,rounds):
 version,m,d,arena=cell
 env=dict(os.environ,KALANDRA_QKV_CONCURRENT='1' if version=='current' else '0')
 if not arena:env['KALANDRA_PROBE_NO_ARENA']='1'
 else:env.pop('KALANDRA_PROBE_NO_ARENA',None)
 rows=p.observations(p.run([b/'profile-arena',bundle,m,d,count,rounds,0,0,0,2,1],env=env),m*d*4,count,rounds)
 assert all(r['gpu_ns']>0 and r['encoding_ns']>0 for r in rows)
 return rows
result=p.measure_paired([(v,m,512,arena) for m in (16,64) for arena in (0,1) for v in ('baseline','current')],invoke,lambda c:c[1]*c[2]*4,blocks=5,target_ms=50,adaptive=True,max_iterations=256)
result.update(acceptance=False,contract=__doc__,warning='Diagnostic profile; runtime prototype numerical outputs separately qualified. No numerical policy changed. Parameter storage is an existing option; no production change.')
p.evidence(p.ROOT/'benchmarks/results/gpu-independent-dispatch-arena-profile-2026-10-29.json',result,[Path(__file__),b/'ProfileArena.cpp',b/'manifest.json'],[b/'profile-arena',b/'metal.o',*bundle.glob('*.metallib')],p.ROOT/'build/native/native/kalandra')
