"""Untouched production control for the isolated backing-reuse diagnostic."""
import argparse,json,os,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'));import protocol as p
ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--study',choices=('off','on'),required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--blocks',type=int,default=3);a=ap.parse_args();assert not a.output.exists()
frozen=p.ROOT/'benchmarks/.build/gpu-grid-production-2026-10-29';probe=p.ROOT/'benchmarks/.build/gpu-backing-reuse-v2-2026-10-29'
artifacts={}
for b in (frozen,probe):
 manifest=json.loads((b/'manifest.json').read_text())
 for field in ('signature','artifacts'):
  for name,h in manifest[field].items():assert p.sha(p.ROOT/name)==h,name
 artifacts.update(manifest['artifacts'])
q=probe/'qualification.json';qualified=json.loads(q.read_text());assert len(qualified['checks'])==36 and all(r['bit_identical'] for r in qualified['checks'])
for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):assert os.getenv(flag) in (None,'','0')
cache=[]
def invoke(cell,count,rounds):
 version,m,d,mode=cell;enabled=version=='current' and a.study=='on'
 exe=(frozen if version=='baseline' else probe)/'gpu'
 rows=[json.loads(s) for s in p.run([exe,frozen/'bundle',m,d,count,rounds,mode,0,0,2,1],env=dict(os.environ,KALANDRA_REUSE_PROBE='1' if enabled else '0')).splitlines()]
 summary=[s for s in rows if s.get('reuse_summary')]
 assert len(summary)==(rounds+2 if version=='current' else 0)
 assert all(s['peak_cached_bytes']<=64*1024*1024 and ((s['hits']>0)==enabled) for s in summary if s['round']>=0)
 cache.append(dict(cell=cell,iterations=count,samples=summary))
 measured=p.observations('\n'.join(json.dumps(s) for s in rows if not s.get('reuse_summary')),m*d*4,count,rounds)
 assert all(s['gpu_ns']==s['encoding_ns']==s['dispatches']==0 for s in measured)
 return measured
shapes=[(1,512),(16,512),(64,512),(64,256)]
data=p.measure_paired([(v,m,d,mode) for m,d in shapes for mode in (0,1,2) for v in ('baseline','current')],invoke,lambda c:c[1]*c[2]*4,blocks=a.blocks,target_ms=100,adaptive=True,max_iterations=512)
data.update(contract=__doc__,production_change=False,acceptance=False,study=a.study,qualification_sha256=p.sha(q),cache_observations=cache)
p.evidence(a.output,data,[Path(__file__),probe/'manifest.json',frozen/'manifest.json',Path(__file__).parent/'backing_reuse_probe.py'],[p.ROOT/name for name in artifacts],p.ROOT/'build/native/native/kalandra')
