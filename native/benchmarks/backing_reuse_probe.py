"""Diagnostic-only bounded backing reuse; NOT production resource semantics.

A process-local 64-entry/64-MiB cache retains idle Metal buffers outside runtime
live-byte statistics. Only an explicit single-caller quiescent boundary after a
complete invocation makes retired buffers reusable. Never use this probe as a
production allocator or claim its budget/fault/concurrency contract is qualified.
"""
import argparse,json,os,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import transformer_reference as ref
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--phase',choices=('prepare','measure'),required=True);parser.add_argument('--build',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--blocks',type=int,default=7)
a=parser.parse_args();assert not a.output.exists();b=a.build.resolve();manifest=b/'manifest.json'
frozen=p.ROOT/'benchmarks/.build/gpu-grid-production-2026-10-29';bundle=frozen/'bundle';fair=Path(__file__).parent/'fair';source=p.ROOT/'native/metal/Metal.mm';original=fair/'TransformerClosedMetal.cpp';task=p.ROOT/'build/native/native/libkalandra-task-core-optimized.a'
sources=[Path(__file__),source,original,fair/'TransformerBlock.h',fair/'ResidualFfn.h',frozen/'TransformerProgram.h',frozen/'manifest.json',Path(ref.__file__)]
signature={p.key(f.resolve()):p.sha(f) for f in sources}
if a.phase=='prepare':
 b.mkdir(parents=True,exist_ok=True);assert not manifest.exists()
 saved=json.loads((frozen/'manifest.json').read_text())
 for field in ('signature','artifacts'):
  for name,h in saved[field].items():assert p.sha(p.ROOT/name)==h,name
 helper=r'''
#include <cstdlib>
// Experimental cache accounting is separate from the production budget ledger.
namespace {
struct ReuseSlot { id<MTLBuffer> buffer=nil; id<MTLDevice> device=nil; uint64_t bytes=0; MTLStorageMode mode=MTLStorageModeShared; bool ready=false; };
static std::array<ReuseSlot,64> reuseSlots{};
static std::mutex reuseMutex;
static uint64_t reuseBytes=0,reusePeak=0,reuseHits=0,reuseMisses=0;
static bool reuseEnabled() { static const bool enabled=[] {auto e=std::getenv("KALANDRA_REUSE_PROBE");return e && std::strcmp(e,"1")==0;}();return enabled; }
static id<MTLBuffer> reuseTake(id<MTLDevice> device,uint64_t n,bool shared) {
  if(reuseEnabled()) {
    std::lock_guard lock(reuseMutex);
    for(auto &slot:reuseSlots) if(slot.ready && slot.buffer && slot.device==device && slot.bytes==n && slot.mode==(shared?MTLStorageModeShared:MTLStorageModePrivate)) {
      auto result=slot.buffer;reuseBytes-=slot.bytes;slot={};++reuseHits;return result;
    }
    ++reuseMisses;
  }
  return [device newBufferWithLength:n options:shared?MTLResourceStorageModeShared:MTLResourceStorageModePrivate];
}
static void reuseRetire(id<MTLBuffer> buffer) {
  if(!reuseEnabled() || !buffer)return;
  std::lock_guard lock(reuseMutex);
  const uint64_t n=buffer.length;
  if(n>64*1024*1024 || reuseBytes>64*1024*1024-n)return;
  for(auto &slot:reuseSlots) if(!slot.buffer) {
    slot={buffer,buffer.device,n,buffer.storageMode,false};reuseBytes+=n;reusePeak=std::max(reusePeak,reuseBytes);return;
  }
}
}
extern "C" void reuseBoundary() {
  if(!reuseEnabled())return;
  std::lock_guard lock(reuseMutex);
  // The driver proves there are no outstanding commands/tasks before this call.
  for(auto &slot:reuseSlots)if(slot.buffer)slot.ready=true;
}
extern "C" uint64_t reuseStat(unsigned i) {
  std::lock_guard lock(reuseMutex);
  return i==0?reuseHits:i==1?reuseMisses:i==2?reuseBytes:reusePeak;
}
'''
 text=source.read_text().replace('namespace kalandra::runtime::metal {',helper+'\nnamespace kalandra::runtime::metal {',1)
 old='''    buffer = [state->devices[index]
        newBufferWithLength:n
                    options:(staging || shared) ? MTLResourceStorageModeShared
                                                : MTLResourceStorageModePrivate];'''
 assert text.count(old)==1;text=text.replace(old,'    buffer = reuseTake(state->devices[index],n,staging || shared);')
 old='    buffer = nil;\n    state->bytes -= bytes;';assert text.count(old)==1
 text=text.replace(old,'    reuseRetire(buffer);\n'+old)
 (b/'Metal.mm').write_text(text)
 driver='extern "C" void reuseBoundary();\nextern "C" unsigned long long reuseStat(unsigned);\n'+original.read_text()
 begin=driver.index('    for (unsigned i = 0; i < 7; ++i) {');end=driver.index('    const auto baseline',begin)
 init=driver[begin:end]
 driver=driver[:begin]+'    auto initialize = [&] {\n'+init+'    };\n    initialize();\n'+driver[end:]
 old='''    if (dump)
      call(true);''';assert driver.count(old)==1
 driver=driver.replace(old,'''    if (dump) {
      for(unsigned step=0;step<4;++step) {
        if(step) {resident={};host={};data=transformer::Inputs(rows,d,(pattern+step)%2);initialize();}
        call(true);
        ffn::check(!runtime.stats().inFlight);
        reuseBoundary();
      }
      std::printf("{\\\"reuse_summary\\\":true,\\\"hits\\\":%llu,\\\"misses\\\":%llu,\\\"cached_bytes\\\":%llu,\\\"peak_cached_bytes\\\":%llu}\\n",reuseStat(0),reuseStat(1),reuseStat(2),reuseStat(3));
    }''')
 old='''        for (unsigned i = 0; i < count; ++i)
          checksum += call(false);''';assert driver.count(old)==1
 driver=driver.replace(old,'''        for (unsigned i = 0; i < count; ++i) {
          checksum += call(false);
          ffn::check(!runtime.stats().inFlight);
          reuseBoundary();
        }''')
 needle='        auto after = runtime.stats();';assert driver.count(needle)==1
 driver=driver.replace(needle,needle+'''\n        std::printf("{\\\"reuse_summary\\\":true,\\\"round\\\":%d,\\\"hits\\\":%llu,\\\"misses\\\":%llu,\\\"cached_bytes\\\":%llu,\\\"peak_cached_bytes\\\":%llu}\\n",round,reuseStat(0),reuseStat(1),reuseStat(2),reuseStat(3));''')
 (b/'Driver.cpp').write_text(driver)
 p.run([p.CLANG,'-std=c++23','-O2','-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs','-mmacosx-version-min=26.0','-I'+str(p.ROOT/'native/metal'),'-c',b/'Metal.mm','-o',b/'metal.o'])
 p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0','-I'+str(frozen),'-I'+str(p.ROOT/'native/metal'),'-I'+str(fair.resolve()),b/'Driver.cpp',b/'metal.o',task,'-framework','Foundation','-framework','Metal','-o',b/'gpu'])
 manifest.write_text(json.dumps(dict(signature=signature,artifacts={p.key(f):p.sha(f) for f in [b/'gpu',b/'metal.o',b/'Metal.mm',b/'Driver.cpp',task,*bundle.glob('*.metallib')]},commands=p.COMMANDS),indent=2)+'\n')
saved=json.loads(manifest.read_text());assert signature==saved['signature']
for name,h in saved['artifacts'].items():assert p.sha(p.ROOT/name)==h,name
shapes=[(1,512),(16,512),(64,512),(64,256)]
if a.phase=='prepare':
 checks=[];oracles={}
 for m,d in shapes:
  for policy in (0,1,2):
   for mode in (0,1,2):
    def invoke(enabled):
     out=p.run([b/'gpu',bundle,m,d,1,1,mode,1,0,policy,1],env=dict(os.environ,KALANDRA_REUSE_PROBE=str(enabled),MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1'))
     rows=[json.loads(s) for s in out.splitlines()];assert len(rows)==5 and rows[-1]['reuse_summary']
     assert rows[-1]['peak_cached_bytes']<=64*1024*1024
     assert (rows[-1]['hits']>0)==bool(enabled)
     return rows
    off,on=invoke(0),invoke(1)
    for step in range(4):
     def bits(row):return struct.pack('<'+'f'*len(row['cells']),*row['cells'])
     assert bits(off[step])==bits(on[step]),(m,d,policy,mode,step)
     key=(m,d,step%2)
     if key not in oracles:
      actual=json.loads(p.run([frozen/'gpu',bundle,m,d,1,1,mode,1,step%2,policy,1],env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')))
      assert bits(actual)==bits(on[step])
      metrics,_=ref.assess(m,d,step%2,on[step]);oracles[key]=(bits(on[step]),metrics)
     assert bits(on[step])==oracles[key][0]
    checks.append(dict(m=m,d=d,mode=mode,policy=policy,changing_output_cases=4,bit_identical=True,pool=on[-1]))
  print(m,d,'changing inputs: all modes/policies passed',flush=True)
 data=dict(contract=__doc__,production_change=False,checks=checks,oracles=[dict(shape=k,metrics=v[1]) for k,v in oracles.items()])
else:
 q=b/'qualification.json';qualification=json.loads(q.read_text());assert len(qualification['checks'])==36 and all(c['bit_identical'] for c in qualification['checks'])
 cache_observations=[]
 for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):assert os.getenv(flag) in (None,'','0')
 def invoke(cell,count,rounds):
  version,m,d,mode=cell;out=p.run([b/'gpu',bundle,m,d,count,rounds,mode,0,0,2,1],env=dict(os.environ,KALANDRA_REUSE_PROBE='1' if version=='current' else '0'))
  rows=[json.loads(s) for s in out.splitlines()];cache=[s for s in rows if s.get('reuse_summary')]
  assert len(cache)==rounds+2 and all(s['peak_cached_bytes']<=64*1024*1024 for s in cache)
  assert all((s['hits']>0)==(version=='current') for s in cache if s['round']>=0)
  cache_observations.append(dict(cell=cell,iterations=count,samples=cache))
  measured=p.observations('\n'.join(json.dumps(s) for s in rows if not s.get('reuse_summary')),m*d*4,count,rounds)
  assert all(s['gpu_ns']==s['encoding_ns']==s['dispatches']==0 for s in measured)
  return measured
 data=p.measure_paired([(v,m,d,mode) for m,d in shapes for mode in (0,1,2) for v in ('baseline','current')],invoke,lambda c:c[1]*c[2]*4,blocks=a.blocks,target_ms=100,adaptive=True,max_iterations=512)
 data.update(contract=__doc__,production_change=False,qualification_sha256=p.sha(q),cache_observations=cache_observations)
p.evidence(a.output,data,sources+[manifest],[p.ROOT/f for f in saved['artifacts']],p.ROOT/'build/native/native/kalandra')
