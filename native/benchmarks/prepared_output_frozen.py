"""Uninstrumented frozen accepted-runtime versus production output-cache comparison.

Same compiled shaders, compiler-generated prepared program, task archive, source
algorithm and all per-invocation work. All outputs are separately qualified by
prepared_output_cache.py. Fresh-process ABBA/BAAB complements its same-runtime
controls; neither removes unfavorable observations in the other.
"""
import argparse,json,os,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--phase',choices=('prepare','measure'),required=True)
ap.add_argument('--build',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
ap.add_argument('--study',choices=('aa','production','repeat','uncached'),default='production')
ap.add_argument('--runtime-build',type=Path,default=p.ROOT/'benchmarks/.build/gpu-prepared-output-cache-2026-10-29')
ap.add_argument('--aa-all',action='store_true',help='Control all four frozen workload cases')
a=ap.parse_args();b=a.build.resolve();assert not a.output.exists()
f=p.ROOT/'benchmarks/.build/gpu-upload-cache-atomic-2026-10-29'
production=a.runtime_build.resolve()
sources=[Path(__file__),Path(p.__file__),production/'manifest.json']
sig={p.key(x.resolve()):p.sha(x) for x in sources}
manifest=b/'manifest.json'
if a.phase=='prepare':
 b.mkdir(parents=True,exist_ok=False)
 s=(f/'Driver.cpp').read_text().replace('m::Options options;', 'm::Options options;\n  options.cachePreparedOutputs = !std::getenv("KALANDRA_NO_OUTPUT_CACHE");')
 for obj in ('after','s'):
  for field,extra in [('cachedUploadBytes','cachedPreparedOutputBytes'),('cachedUploadBuffers','cachedPreparedOutputBuffers'),('uploadCacheHits','preparedOutputCacheHits'),('uploadCacheMisses','preparedOutputCacheMisses')]:
   s=s.replace(obj+'.'+field,'('+obj+'.'+field+' + '+obj+'.'+extra+')')
 s=s.replace('options.maxCachedUploadBytes &&','options.maxCachedDeviceBytes &&')
 (b/'Driver.cpp').write_text(s)
 p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0','-I'+str(production),'-I'+str(f),'-I'+str(p.ROOT/'native/metal'),'-I'+str(p.ROOT/'native/benchmarks/fair'),b/'Driver.cpp',production/'metal.o',f/'candidate-task.a','-framework','Foundation','-framework','Metal','-o',b/'gpu'])
 files=[b/'Driver.cpp',b/'gpu',production/'Metal.h',production/'Metal.mm',production/'metal.o',f/'candidate',f/'candidate-task.a',f/'TransformerProgram.h',*(f/'bundle').glob('*.metallib')]
 manifest.write_text(json.dumps(dict(signature=sig,artifacts={p.key(x):p.sha(x) for x in files},commands=p.COMMANDS),indent=2)+'\n');a.output.write_text(manifest.read_text());sys.exit(0)
frozen=json.loads(manifest.read_text());assert frozen['signature']==sig
for name,digest in frozen['artifacts'].items():assert p.sha(p.ROOT/name)==digest,name
q=production/'qualification.json';qualification=json.loads(q.read_text());assert len(qualification['checks'])==486 and all(x['passed'] for x in qualification['checks'])
for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):assert os.getenv(flag) in (None,'','0')
caches=[]
def invoke(cell,count,rounds):
 version,m,d,mode=cell
 exe=b/'gpu' if version=='current' or a.study=='aa' else f/'candidate'
 env=dict(os.environ)
 if a.study=='uncached':env['KALANDRA_NO_OUTPUT_CACHE']='1'
 rows=[json.loads(x) for x in p.run([exe,f/'bundle',m,d,count,rounds,mode,0,0,2,1],env=env).splitlines()]
 cache=[r for r in rows if r.get('cache')];timed=[r for r in rows if 'ns' in r]
 caches.append(dict(cell=cell,iterations=count,samples=cache))
 for c in cache:assert c['retained_bytes']==c['live_bytes']+c['cached_bytes'] and c['cached_bytes']<=16*1024*1024 and c['peak_retained_bytes']<=64*1024*1024
 return p.observations('\n'.join(json.dumps(r) for r in timed),m*d*4,count,rounds)
groups=[(1,512,0),(64,512,0),(512,512,0),(512,512,2)]
if a.study=='aa' and not a.aa_all:groups=[(512,512,0),(512,512,2)]
cells=[[v,*g] for g in groups for v in ('baseline','current')]
data=p.measure_paired(cells,invoke,lambda cell:cell[1]*cell[2]*4,blocks=3,rounds=3,target_ms=1000,adaptive=True,max_iterations=8192)
data.update(contract=__doc__,study=a.study,cache_observations=caches,qualification_sha256=p.sha(q))
p.evidence(a.output,data,sources+[manifest],[p.ROOT/x for x in frozen['artifacts']],p.ROOT/'build/native/native/kalandra')
