"""Diagnostic private-output reuse inside the accepted bounded cache.

Copied runtime only. Eligible buffers are full float32 outputs of validated pure
prepared progression. Retirement requires zero admissions and last ownership;
no pending resource is cached. Every successful eligible operation overwrites
all physical bytes. Scratch, flags, parameters, empty outputs, transforms and
ordinary/custom kernel execution keep original allocation. The probe shares the
16 MiB/64-entry idle cap with uploads; counters named upload include both kinds
only in this diagnostic. This is not production API or qualification.
"""
import argparse,json,os,shutil,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import transformer_reference as ref
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--phase',choices=('prepare','verify','measure'),required=True)
ap.add_argument('--build',type=Path,required=True);ap.add_argument('--frozen',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
ap.add_argument('--study',choices=('screen','aa','production','repeat'),default='screen');ap.add_argument('--blocks',type=int,default=3)
a=ap.parse_args();b=a.build.resolve();f=a.frozen.resolve();assert not a.output.exists()
manifest=b/'manifest.json';fair=Path(__file__).parent/'fair';source=p.ROOT/'native/metal/Metal.mm'
sources=[Path(__file__),source,p.ROOT/'native/metal/Metal.h',fair/'TransformerClosedMetal.cpp',fair/'TransformerBlock.h',fair/'ResidualFfn.h',Path(p.__file__),Path(ref.__file__),f/'candidate.json']
signature={p.key(x.resolve()):p.sha(x) for x in sources}
if a.phase=='prepare':
 b.mkdir(parents=True,exist_ok=False)
 frozen=json.loads((f/'candidate.json').read_text())
 for field in ('signature','artifacts'):
  for path,digest in frozen[field].items():assert p.sha(p.ROOT/path)==digest,path
 text=source.read_text()
 def replace(old,new):
  global text
  assert text.count(old)==1,(old,text.count(old));text=text.replace(old,new)
 text=text.replace('namespace kalandra::runtime::metal {','''namespace kalandra::runtime::metal {
static bool privateProbeEnabled() { static const bool enabled=[] { auto e=std::getenv("KALANDRA_PRIVATE_OUTPUT_PROBE");return e && std::strcmp(e,"1")==0; }();return enabled; }
''',1)
 replace('    std::uint32_t device = 0;','    std::uint32_t device = 0;\n    bool privateOutput = false;')
 replace('id<MTLBuffer> __strong &buffer) noexcept {','id<MTLBuffer> __strong &buffer, bool privateOutput = false) noexcept {')
 replace('entry.size == n && entry.device == device)', 'entry.size == n && entry.device == device && entry.privateOutput == privateOutput)')
 replace('std::uint32_t device) noexcept {','std::uint32_t device, bool privateOutput) noexcept {')
 replace('entry = {buffer, n, device};','entry = {buffer, n, device, privateOutput};')
 replace('  bool cacheableUpload = false;','  bool cacheableUpload = false;\n  bool privateOutput = false;')
 replace('bool stage, bool upload, std::uint32_t index)','bool stage, bool upload, std::uint32_t index, bool output)')
 replace('cacheableUpload(upload), deviceIndex(index)', 'cacheableUpload(upload), privateOutput(output), deviceIndex(index)')
 replace('state->retire(buffer, bytes, cacheableUpload, deviceIndex);','state->retire(buffer, bytes, cacheableUpload, deviceIndex, privateOutput);')
 replace('bool staging, bool shared = false) {','bool staging, bool shared = false, bool fullOutput = false) {')
 replace('const bool upload = shared && !staging;','const bool upload = (shared && !staging) || fullOutput;')
 replace('state->reserveUpload(n, index, buffer)', 'state->reserveUpload(n, index, buffer, fullOutput)')
 replace('n, staging, upload, index);','n, staging, upload, index, fullOutput);')
 replace('bool device, std::uint32_t index = 0, bool shared = false) {','bool device, std::uint32_t index = 0, bool shared = false, bool fullOutput = false) {')
 replace('allocateMetal(state, index, physical, false, shared);','allocateMetal(state, index, physical, false, shared, fullOutput);')
 replace('state->devices[index].hasUnifiedMemory);','''state->devices[index].hasUnifiedMemory,
                                      privateProbeEnabled() && (compute || matrix) && f.progression &&
                                          f.packet.kernel.state_->numericType == NumericType::Float32);''')
 (b/'Metal.mm').write_text(text)
 driver=(f/'Driver.cpp').read_text();verify=(f/'Verify.cpp').read_text()
 # Only stats code differs from the original hot loop. Cache bytes now include outputs.
 for name,body in [('Driver.cpp',driver),('Verify.cpp',verify)]: (b/name).write_text(body)
 obj=b/'metal.o';header=p.ROOT/'native/metal';task=f/'candidate-task.a'
 p.run([p.CLANG,'-std=c++23','-O2','-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs','-mmacosx-version-min=26.0','-I'+str(header),'-c',b/'Metal.mm','-o',obj])
 for name,exe in [('Driver.cpp','gpu'),('Verify.cpp','verify')]:
  p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0','-I'+str(f),'-I'+str(header),'-I'+str(fair.resolve()),b/name,obj,task,'-framework','Foundation','-framework','Metal','-o',b/exe])
 artifacts=[b/'gpu',b/'verify',obj,b/'Metal.mm',b/'Driver.cpp',b/'Verify.cpp',task,f/'candidate',*(f/'bundle').glob('*.metallib')]
 manifest.write_text(json.dumps(dict(signature=signature,artifacts={p.key(x):p.sha(x) for x in artifacts},commands=p.COMMANDS),indent=2)+'\n')
 a.output.write_text(manifest.read_text());sys.exit(0)
saved=json.loads(manifest.read_text());assert signature==saved['signature']
for path,digest in saved['artifacts'].items():assert p.sha(p.ROOT/path)==digest,path
shapes=[(1,512),(16,512),(64,512),(64,256)]
def decode(s):return json.loads(s,parse_int=lambda n:-0.0 if n=='-0' else int(n))
def bits(row):return struct.pack('<'+'f'*len(row['cells']),*row['cells'])
if a.phase=='verify':
 checks=[];oracles={}
 for m,d in shapes:
  expected={}
  for pattern in (0,1):
   row=decode(p.run([f/'candidate',f/'bundle',m,d,1,1,0,1,pattern,2,1],env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')))
   metrics,_=ref.assess(m,d,pattern,row);expected[pattern]=bits(row);oracles[m,d,pattern]=metrics
  for enabled in (0,1):
   for policy in (0,1,2):
    for mode in (0,1,2):
     out=p.run([b/'verify',f/'bundle',m,d,1,1,mode,1,0,policy,1],env=dict(os.environ,KALANDRA_PRIVATE_OUTPUT_PROBE=str(enabled),MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1'))
     rows=[decode(s) for s in out.splitlines()];assert len(rows)==4
     assert all(bits(row)==expected[i%2] for i,row in enumerate(rows))
     checks.append(dict(m=m,d=d,enabled=enabled,policy=policy,mode=mode,comparisons=4,passed=True))
  print(m,d,'full outputs and oracle passed',flush=True)
 data=dict(contract=__doc__,checks=checks,oracles=[dict(shape=k,metrics=v) for k,v in oracles.items()])
else:
 q=b/'qualification.json';qualified=json.loads(q.read_text());assert len(qualified['checks'])==72 and all(x['passed'] for x in qualified['checks'])
 for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):assert os.getenv(flag) in (None,'','0')
 cache=[]
 def invoke(cell,count,rounds):
  version,m,d,mode=cell;production=version=='baseline' and a.study in ('production','repeat')
  enabled=int(version=='current' and a.study!='aa')
  exe=f/'candidate' if production else b/'gpu'
  rows=[json.loads(s) for s in p.run([exe,f/'bundle',m,d,count,rounds,mode,0,0,2,1],env=dict(os.environ,KALANDRA_PRIVATE_OUTPUT_PROBE=str(enabled))).splitlines()]
  extra=[x for x in rows if x.get('cache')];assert len(extra)==rounds+2
  for x in extra:
   assert x['retained_bytes']==x['live_bytes']+x['cached_bytes'] and x['peak_retained_bytes']<=64*1024*1024 and x['cached_bytes']<=16*1024*1024
   if mode==0 and (not enabled or production):assert x['hits']==x['cached_bytes']==0
   if mode==0 and enabled and not production and x['round']>=0:assert x['hits']>0
  cache.append(dict(cell=cell,iterations=count,samples=extra))
  return p.observations('\n'.join(json.dumps(x) for x in rows if not x.get('cache')),m*d*4,count,rounds)
 data=p.measure_paired([(v,m,d,mode) for m,d in shapes for mode in (0,1,2) for v in ('baseline','current')],invoke,lambda c:c[1]*c[2]*4,blocks=a.blocks,target_ms=250,adaptive=True,max_iterations=2048)
 data.update(contract=__doc__,study=a.study,cache_observations=cache,qualification_sha256=p.sha(q))
p.evidence(a.output,data,sources+[manifest],[p.ROOT/x for x in saved['artifacts']],p.ROOT/'build/native/native/kalandra')
