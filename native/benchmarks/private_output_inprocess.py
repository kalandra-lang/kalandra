"""Diagnostic same-runtime paired intervals for private-output reuse.

One runtime/queue/prepared program and resident operands survive all ABBA/BAAB
slots. Trim idle storage and run two complete warm-up intervals before every
measured interval. Switching happens only at a quiescent boundary. GPU and
encoding counters are enabled equally on both sides. Production is untouched.
"""
import argparse,json,os,statistics as st,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import transformer_reference as ref
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--phase',choices=('prepare','verify','measure'),required=True)
ap.add_argument('--build',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
ap.add_argument('--study',choices=('aa','toggle','repeat'),default='aa')
ap.add_argument('--scope',choices=('largest','broad'),default='largest')
ap.add_argument('--count',type=int,default=1024);ap.add_argument('--blocks',type=int,default=5)
a=ap.parse_args();b=a.build.resolve();assert not a.output.exists()
f=p.ROOT/'benchmarks/.build/gpu-upload-cache-atomic-2026-10-29';old=p.ROOT/'benchmarks/.build/gpu-private-output-probe-2026-10-29'
fair=Path(__file__).parent/'fair';manifest=b/'manifest.json'
sources=[Path(__file__),Path(p.__file__),Path(ref.__file__),old/'manifest.json',p.ROOT/'native/metal/Metal.h',fair/'TransformerClosedMetal.cpp']
sig={p.key(x.resolve()):p.sha(x) for x in sources}
if a.phase=='prepare':
 b.mkdir(parents=True,exist_ok=False)
 saved=json.loads((old/'manifest.json').read_text())
 for key in ('signature','artifacts'):
  for name,digest in saved[key].items():assert p.sha(p.ROOT/name)==digest,name
 text=(old/'Metal.mm').read_text()
 start=text.index('static bool privateProbeEnabled()');end=text.index('\n',start)
 text=text[:start]+'''static std::atomic<bool> pairEnabled{false};
extern "C" void pairSetEnabled(bool enabled) { pairEnabled.store(enabled); }
static bool privateProbeEnabled() { return pairEnabled.load(); }'''+text[end:]
 (b/'Metal.mm').write_text(text)
 prefix='extern "C" void pairSetEnabled(bool);\n'
 def common(body):
  return prefix+body.replace('m::Options options;', '''m::Options options;
  options.profile = true;
  pairSetEnabled(std::getenv("KALANDRA_PRIVATE_OUTPUT_PROBE") != nullptr);''')
 driver=common((f/'Driver.cpp').read_text())
 needle='        auto before = runtime.stats();';assert driver.count(needle)==1
 driver=driver.replace(needle,'''        const unsigned block = round < 0 ? 0 : unsigned(round) / 4;
        const unsigned slot = round < 0 ? 0 : unsigned(round) % 4;
        const bool current = block % 2 ? slot == 0 || slot == 3 : slot == 1 || slot == 2;
        const bool enabled = current && !std::getenv("KALANDRA_PAIR_CONTROL");
        ffn::check(!runtime.stats().inFlight);
        runtime.trimUploadCache();
        pairSetEnabled(enabled);
        for (unsigned warm = 0; warm < 2; ++warm)
          for (unsigned i = 0; i < count; ++i) call(false);
        std::printf("{\\\"pair_state\\\":true,\\\"round\\\":%d,\\\"current\\\":%s,\\\"enabled\\\":%s}\\n",round,current?"true":"false",enabled?"true":"false");
'''+needle)
 (b/'Driver.cpp').write_text(driver)
 verify=common((f/'Verify.cpp').read_text())
 needle='        call(true);';assert verify.count(needle)==1
 verify=verify.replace(needle,'''        ffn::check(!runtime.stats().inFlight);
        runtime.trimUploadCache();
        pairSetEnabled((step % 2 == 0) != (std::getenv("KALANDRA_PAIR_REVERSE") != nullptr));
        // Warm backing with opposite contents, then verify a fresh input.
        resident={};host={};data=transformer::Inputs(rows,d,(pattern+step+1)%2);initialize();
        call(false);
        resident={};host={};data=transformer::Inputs(rows,d,(pattern+step)%2);initialize();
'''+needle)
 (b/'Verify.cpp').write_text(verify)
 p.run([p.CLANG,'-std=c++23','-O2','-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs','-mmacosx-version-min=26.0','-I'+str(p.ROOT/'native/metal'),'-c',b/'Metal.mm','-o',b/'metal.o'])
 for source,exe in [('Driver.cpp','gpu'),('Verify.cpp','verify')]:
  p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0','-I'+str(f),'-I'+str(p.ROOT/'native/metal'),'-I'+str(fair.resolve()),b/source,b/'metal.o',f/'candidate-task.a','-framework','Foundation','-framework','Metal','-o',b/exe])
 files=[b/'Metal.mm',b/'Driver.cpp',b/'Verify.cpp',b/'metal.o',b/'gpu',b/'verify',f/'candidate-task.a',f/'candidate',f/'TransformerProgram.h',*(f/'bundle').glob('*.metallib')]
 manifest.write_text(json.dumps(dict(signature=sig,artifacts={p.key(x):p.sha(x) for x in files},commands=p.COMMANDS),indent=2)+'\n')
 a.output.write_text(manifest.read_text());sys.exit(0)
saved=json.loads(manifest.read_text());assert sig==saved['signature']
for name,digest in saved['artifacts'].items():assert p.sha(p.ROOT/name)==digest,name
if a.phase=='verify':
 decode=lambda s:json.loads(s,parse_int=lambda n:-0.0 if n=='-0' else int(n))
 bits=lambda row:struct.pack('<'+'f'*len(row['cells']),*row['cells'])
 checks=[];oracles=[]
 for m,d in [(1,512),(16,512),(64,512),(64,256),(3,33),(7,65),(1,17),(31,31)]:
  expected={}
  for pattern in (0,1):
   row=decode(p.run([f/'candidate',f/'bundle',m,d,1,1,0,1,pattern,2,1],env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')))
   metrics,_=ref.assess(m,d,pattern,row);expected[pattern]=bits(row);oracles.append(dict(m=m,d=d,pattern=pattern,metrics=metrics))
  for reverse in (False,True):
   for policy in (0,1,2):
    for mode in (0,1,2):
     env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')
     if reverse:env['KALANDRA_PAIR_REVERSE']='1'
     rows=[decode(s) for s in p.run([b/'verify',f/'bundle',m,d,1,1,mode,1,0,policy,1],env=env).splitlines()]
     assert len(rows)==4 and all(bits(row)==expected[i%2] for i,row in enumerate(rows))
     checks.append(dict(m=m,d=d,reverse=reverse,policy=policy,mode=mode,comparisons=4,passed=True))
  print(m,d,'alternating enable/disable full outputs passed',flush=True)
 data=dict(contract=__doc__,checks=checks,oracles=oracles)
else:
 q=b/'qualification.json';qualified=json.loads(q.read_text());assert len(qualified['checks'])==144 and all(x['passed'] for x in qualified['checks'])
 for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):assert os.getenv(flag) in (None,'','0')
 assert a.count>0 and a.blocks>0
 groups=[(64,512,0)] if a.scope=='largest' else [(1,512,0),(16,512,0),(64,256,0),(64,512,0),(64,512,2)]
 observations=[];pairs=[]
 for m,d,mode in groups:
  env=dict(os.environ)
  if a.study=='aa':env['KALANDRA_PAIR_CONTROL']='1'
  output=p.run([b/'gpu',f/'bundle',m,d,a.count,a.blocks*4,mode,0,0,2,1],env=env)
  rows=[json.loads(s) for s in output.splitlines()]
  timed=[r for r in rows if 'ns' in r];cache=[r for r in rows if r.get('cache')];states=[r for r in rows if r.get('pair_state')]
  p.observations('\n'.join(json.dumps(r) for r in timed),m*d*4,a.count,a.blocks*4)
  assert len(cache)==len(states)==len(timed)
  for sample,c,state in zip(timed,cache,states):
   assert c['round']==state['round']==sample['round']
   assert c['retained_bytes']==c['live_bytes']+c['cached_bytes'] and c['cached_bytes']<=16*1024*1024 and c['peak_retained_bytes']<=64*1024*1024
   assert sample['gpu_ns']>0 and sample['encoding_ns']>0
   if mode==0:assert bool(c['cached_bytes'])==state['enabled']
  for block in range(a.blocks):
   selected=[(r,s) for r,s in zip(timed,states) if r['round']>=0 and r['round']//4==block]
   before=[r for r,s in selected if not s['current']];after=[r for r,s in selected if s['current']]
   assert len(before)==len(after)==2
   summary={}
   for field in ('ns','gpu_ns','encoding_ns'):
    oldvalue=st.mean(r[field]/a.count for r in before);newvalue=st.mean(r[field]/a.count for r in after)
    summary[field]=dict(baseline=oldvalue,current=newvalue,reduction=1-newvalue/oldvalue)
   pairs.append(dict(group=[m,d,mode],block=block,metrics=summary))
  observations.append(dict(group=[m,d,mode],iterations=a.count,samples=rows))
  print(m,d,mode,'time reduction',round(st.median(r['metrics']['ns']['reduction']*100 for r in pairs if r['group']==[m,d,mode]),2),'%',flush=True)
 data=dict(contract=__doc__,study=a.study,scope=a.scope,protocol=dict(blocks=a.blocks,iterations=a.count,per_slot_warmup_intervals=2,initial_warmups=2,order='ABBA/BAAB',profile=True),observations=observations,pairs=pairs,qualification_sha256=p.sha(q))
p.evidence(a.output,data,sources+[manifest],[p.ROOT/x for x in saved['artifacts']],p.ROOT/'build/native/native/kalandra')
