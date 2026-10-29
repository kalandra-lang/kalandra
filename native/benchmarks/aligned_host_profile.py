"""Diagnostic production transformer profile after aligned-grid promotion.

Copied runtime scopes are inclusive and overlap. Closed-command submit/start/end/
wake timestamps share the Mach clock domain; this is not performance acceptance.
"""
import argparse,json,os,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
a=parser.parse_args();assert not a.output.exists();b=a.build.resolve();b.mkdir(parents=True,exist_ok=False)
frozen=p.ROOT/'benchmarks/.build/gpu-grid-production-2026-10-29';manifest=frozen/'manifest.json'
saved=json.loads(manifest.read_text())
for field in ('signature','artifacts'):
 for name,h in saved[field].items():assert p.sha(p.ROOT/name)==h,name
source=p.ROOT/'native/metal/Metal.mm';text=source.read_text()
helpers=r'''
#include <mach/mach_time.h>
static double hpNow() {
  static const auto scale=[] { mach_timebase_info_data_t t; mach_timebase_info(&t); return double(t.numer)/t.denom; }();
  return mach_absolute_time()*scale;
}
static std::atomic<uint64_t> hpCounters[12]{};
struct HpScope { unsigned slot; double start=hpNow(); ~HpScope(){hpCounters[slot]+=uint64_t(hpNow()-start);} };
extern "C" uint64_t hpValue(unsigned i) { return hpCounters[i].load(); }
extern "C" void hpReset() { for(auto &v:hpCounters) v=0; }
'''
text=text.replace('namespace kalandra::runtime::metal {',helpers+'\nnamespace kalandra::runtime::metal {',1)
for needle,slot in [('bool staging, bool shared = false) {',0),('~MetalAllocation() {',1),('static id<MTLCommandBuffer> numericCommandBuffer(Frame &f, std::uint32_t index) {',2),('static std::expected<void, Error> prepareProgression(Task &task, Frame &f) {',3),('static void resume(Task &task) {',4),('static void submitCommand(Task &task, Frame &f, id<MTLCommandBuffer> command) {',5)]:
 assert text.count(needle)==1,needle
 text=text.replace(needle,needle+f'\n HpScope timer{{{slot}}};')
needle='      [command commit];\n      [command waitUntilCompleted];';assert text.count(needle)==1
text=text.replace(needle,'''      const double hpSubmit=hpNow();
      [command commit];
      const double hpCommitted=hpNow();
      [command waitUntilCompleted];
      const double hpWake=hpNow();
      hpCounters[6]+=uint64_t(std::max(0.0,command.GPUStartTime*1e9-hpSubmit));
      hpCounters[7]+=uint64_t(std::max(0.0,hpWake-command.GPUEndTime*1e9));
      hpCounters[8]+=uint64_t(std::max(0.0,(command.GPUEndTime-command.GPUStartTime)*1e9));
      ++hpCounters[9];
      hpCounters[10]+=uint64_t(hpCommitted-hpSubmit);
      hpCounters[11]+=uint64_t(hpWake-hpCommitted);''')
(b/'Metal.mm').write_text(text)
fair=Path(__file__).parent/'fair';original=fair/'TransformerClosedMetal.cpp'
driver='extern "C" unsigned long long hpValue(unsigned);\nextern "C" void hpReset();\n'+original.read_text()
driver=driver.replace('m::Options options;','m::Options options;\n  options.profile=true;').replace('auto before = runtime.stats();','hpReset();auto before = runtime.stats();')
needle='const double ns = ffn::elapsed(start);';assert driver.count(needle)==1
driver=driver.replace(needle,needle+'''\nstd::printf("{\\\"host_profile\\\":true,\\\"round\\\":%d,\\\"wall_ns\\\":%.0f,\\\"buckets\\\":[",round,ns);for(unsigned q=0;q<12;++q)std::printf("%s%llu",q?",":"",hpValue(q));std::puts("]}");''')
(b/'Driver.cpp').write_text(driver)
obj=b/'metal.o';binary=b/'profile';task=p.ROOT/'build/native/native/libkalandra-task-core-optimized.a'
p.run([p.CLANG,'-std=c++23','-O2','-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs','-mmacosx-version-min=26.0','-I'+str(p.ROOT/'native/metal'),'-c',b/'Metal.mm','-o',obj])
p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0','-I'+str(frozen),'-I'+str(p.ROOT/'native/metal'),'-I'+str(fair.resolve()),b/'Driver.cpp',obj,task,'-framework','Foundation','-framework','Metal','-o',binary])
shapes=[(1,512),(16,512),(64,512),(64,256)];checks=[];rows=[]
for m,d in shapes:
 for mode in (0,1,2):
  def output(exe):
   row=json.loads(p.run([exe,frozen/'bundle',m,d,1,1,mode,1,0,2,1],env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')))
   return struct.pack('<'+'f'*len(row['cells']),*row['cells'])
  assert output(binary)==output(frozen/'gpu')
  checks.append(dict(m=m,d=d,mode=mode,bit_identical=True))
for trial in range(3):
 for m,d in shapes:
  for mode in (0,1,2):
   count=100;log=p.run([binary,frozen/'bundle',m,d,count,3,mode,0,0,2,1]);samples=[json.loads(s) for s in log.splitlines()]
   timings=p.observations('\n'.join(json.dumps(s) for s in samples if not s.get('host_profile')),m*d*4,count,3)
   hp=[s for s in samples if s.get('host_profile')];assert len(hp)==5 and all(s['buckets'][9]==count for s in hp)
   assert all(s['gpu_ns']>0 and s['encoding_ns']>0 and s['dispatches']>0 for s in timings)
   rows.append(dict(m=m,d=d,mode=mode,trial=trial,iterations=count,samples=samples))
  print(trial,m,d,'profile complete',flush=True)
p.evidence(a.output,dict(contract=__doc__,production_change=False,buckets=['allocateMetal','destroyMetal','commandCreate','prepareProgression','resumeInclusive','submitInclusive','submitToGPUStart','GPUEndToWake','closedGPU','closedCommandCount','closedCommitCPU','closedWaitWall'],checks=checks,observations=rows),[Path(__file__),manifest,source,original,b/'Metal.mm',b/'Driver.cpp',frozen/'TransformerProgram.h'],[binary,obj,task,frozen/'gpu',*(frozen/'bundle').glob('*.metallib')],p.ROOT/'build/native/native/kalandra')
