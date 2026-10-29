"""Actual shared-upload cache qualification and frozen paired/control measurements."""
import argparse,json,os,shutil,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import transformer_reference as ref
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--phase',choices=('prepare','verify','measure'),required=True)
ap.add_argument('--build',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
ap.add_argument('--study',choices=('paired','repeat','aa','toggle','uncached'),default='paired');ap.add_argument('--blocks',type=int,default=7)
a=ap.parse_args();b=a.build.resolve();assert not a.output.exists()
baseline=b/'baseline.json';saved=json.loads(baseline.read_text())
for f,h in saved['files'].items():assert p.sha(p.ROOT/f)==h,f
fair=Path(__file__).parent/'fair';metal=p.ROOT/'build/native/native/metal/libkalandra-metal-optimized.a';task=p.ROOT/'build/native/native/libkalandra-task-core-optimized.a';compiler=p.ROOT/'build/native/native/kalandra'
sources=[Path(__file__),Path(p.__file__),Path(ref.__file__),fair/'TransformerClosedMetal.cpp',fair/'TransformerBlock.h',fair/'ResidualFfn.h',p.ROOT/'native/metal/Metal.mm',p.ROOT/'native/metal/Metal.h',p.ROOT/'native/metal/UploadTests.cpp']
sig={p.key(f.resolve()):p.sha(f) for f in sources};manifest=b/'candidate.json'
if a.phase=='prepare':
 assert not manifest.exists()
 snapshots=[]
 for source in sources:
  target=b/'frozen-sources'/p.key(source.resolve());target.parent.mkdir(parents=True,exist_ok=True)
  shutil.copyfile(source,target);assert p.sha(target)==p.sha(source);snapshots.append(target)
 for source,name in [(metal,'candidate-metal.a'),(task,'candidate-task.a')]:
  shutil.copyfile(source,b/name);assert p.sha(b/name)==p.sha(source)
 metal,task=b/'candidate-metal.a',b/'candidate-task.a'
 driver=(fair/'TransformerClosedMetal.cpp').read_text()
 driver=driver.replace('m::Options options;', '''m::Options options;
  if (auto limit = std::getenv("KALANDRA_UPLOAD_CACHE_BYTES"))
    options.maxCachedUploadBytes = std::stoull(limit);''')
 needle='        auto after = runtime.stats();';assert driver.count(needle)==1
 driver=driver.replace(needle,needle+'''
        ffn::check(after.retainedBytes == after.bytes + after.cachedUploadBytes &&
                   after.peakRetainedBytes <= options.maxBytes &&
                   after.cachedUploadBytes <= options.maxCachedUploadBytes &&
                   after.cachedUploadBuffers <= 64);
        std::printf("{\\\"cache\\\":true,\\\"round\\\":%d,\\\"live_bytes\\\":%llu,\\\"cached_bytes\\\":%llu,\\\"retained_bytes\\\":%llu,\\\"peak_retained_bytes\\\":%llu,\\\"hits\\\":%llu,\\\"misses\\\":%llu}\\n",round,
          (unsigned long long)after.bytes,(unsigned long long)after.cachedUploadBytes,
          (unsigned long long)after.retainedBytes,(unsigned long long)after.peakRetainedBytes,
          (unsigned long long)after.uploadCacheHits,(unsigned long long)after.uploadCacheMisses);''')
 (b/'Driver.cpp').write_text(driver)
 verify=driver;start=verify.index('    for (unsigned i = 0; i < 7; ++i) {');end=verify.index('    const auto baseline',start)
 verify=verify[:start]+'    auto initialize = [&] {\n'+verify[start:end]+'    };\n    initialize();\n'+verify[end:]
 old='''    if (dump)
      call(true);''';assert verify.count(old)==1
 verify=verify.replace(old,'''    if (dump) {
      for(unsigned step=0;step<4;++step) {
        if(step) {resident={};host={};data=transformer::Inputs(rows,d,(pattern+step)%2);initialize();}
        call(true);
        const auto s=runtime.stats();
        ffn::check(!s.inFlight && s.retainedBytes==s.bytes+s.cachedUploadBytes &&
          s.peakRetainedBytes<=options.maxBytes && s.cachedUploadBytes<=options.maxCachedUploadBytes && s.cachedUploadBuffers<=64);
      }
    }''')
 (b/'Verify.cpp').write_text(verify)
 for name,exe in [('Driver.cpp','candidate'),('Verify.cpp','verify')]:
  p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0','-I'+str(b),'-I'+str(p.ROOT/'native/metal'),'-I'+str(fair.resolve()),b/name,metal,task,'-framework','Foundation','-framework','Metal','-o',b/exe])
 artifacts=[b/'candidate',b/'verify',b/'Driver.cpp',b/'Verify.cpp',metal,task,*snapshots]
 manifest.write_text(json.dumps(dict(signature=sig,artifacts={p.key(f):p.sha(f) for f in artifacts},commands=p.COMMANDS),indent=2)+'\n')
 a.output.write_text(manifest.read_text());sys.exit(0)
frozen=json.loads(manifest.read_text());assert sig==frozen['signature']
for f,h in frozen['artifacts'].items():assert p.sha(p.ROOT/f)==h,f
shapes=[(1,512),(16,512),(64,512),(64,256)]
if a.phase=='verify':
 checks=[];oracles={}
 def decode(s):return json.loads(s,parse_int=lambda v:-0.0 if v=='-0' else int(v))
 def bits(row):return struct.pack('<'+'f'*len(row['cells']),*row['cells'])
 for m,d in shapes:
  for pattern in (0,1):
   row=decode(p.run([b/'baseline-gpu',b/'bundle',m,d,1,1,0,1,pattern,2,1],env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')))
   metrics,_=ref.assess(m,d,pattern,row);oracles[m,d,pattern]=(row,metrics)
  for cap in (0,4096,16*1024*1024):
   for mode in (0,1,2):
    for policy in (0,1,2):
     rows=[decode(s) for s in p.run([b/'verify',b/'bundle',m,d,1,1,mode,1,0,policy,1],env=dict(os.environ,KALANDRA_UPLOAD_CACHE_BYTES=str(cap),MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')).splitlines()]
     assert len(rows)==4
     assert all(bits(row)==bits(oracles[m,d,step%2][0]) for step,row in enumerate(rows))
     checks.append(dict(m=m,d=d,cap=cap,mode=mode,policy=policy,comparisons=4,passed=True))
  print(m,d,'all cache limits/modes/policies passed',flush=True)
 data=dict(contract=__doc__,checks=checks,oracles=[dict(shape=k,metrics=v[1]) for k,v in oracles.items()])
else:
 q=b/'qualification.json';qualified=json.loads(q.read_text());assert len(qualified['checks'])==108 and all(r['passed'] for r in qualified['checks'])
 for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):assert os.getenv(flag) in (None,'','0')
 cache=[]
 def invoke(cell,count,rounds):
  version,m,d,mode=cell
  old=version=='baseline' and a.study in ('paired','repeat','uncached')
  cap=16*1024*1024 if version=='current' and a.study not in ('aa','uncached') else 0
  exe=b/('baseline-gpu' if old else 'candidate')
  rows=[json.loads(s) for s in p.run([exe,b/'bundle',m,d,count,rounds,mode,0,0,2,1],env=dict(os.environ,KALANDRA_UPLOAD_CACHE_BYTES=str(cap))).splitlines()]
  extra=[r for r in rows if r.get('cache')]
  assert len(extra)==(0 if old else rounds+2)
  for r in extra:
   assert r['retained_bytes']==r['live_bytes']+r['cached_bytes'] and r['peak_retained_bytes']<=64*1024*1024 and r['cached_bytes']<=cap
   if not cap or mode==0:assert r['hits']==r['cached_bytes']==0
   elif r['round']>=0:assert r['hits']>0
  cache.append(dict(cell=cell,iterations=count,samples=extra))
  timed=p.observations('\n'.join(json.dumps(r) for r in rows if not r.get('cache')),m*d*4,count,rounds)
  assert all(r['gpu_ns']==r['encoding_ns']==r['dispatches']==0 for r in timed)
  return timed
 data=p.measure_paired([(v,m,d,mode) for m,d in shapes for mode in (0,1,2) for v in ('baseline','current')],invoke,lambda c:c[1]*c[2]*4,blocks=a.blocks,target_ms=150,adaptive=True,max_iterations=512)
 data.update(contract=__doc__,study=a.study,cache_observations=cache,qualification_sha256=p.sha(q))
p.evidence(a.output,data,sources+[manifest,baseline],[p.ROOT/f for f in frozen['artifacts']]+[b/'baseline-gpu',*(b/'bundle').glob('*.metallib')],compiler)
