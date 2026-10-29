"""Diagnostic-only Metal host/driver timing; does not modify production sources.

Mach timestamps share Metal GPUStartTime/GPUEndTime's clock domain:
https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime
Buckets are inclusive, overlap, and must not be summed into exclusive wall time.
"""
import argparse,json,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();assert not args.output.exists()
build=args.build.resolve();build.mkdir(parents=True,exist_ok=True)
accept=p.ROOT/'benchmarks/.build/gpu-work-chunks-2026-10-29/acceptance'
bundle=p.ROOT/'benchmarks/.build/gpu-attention-2026-10-29/acceptance/bundle'
source=p.ROOT/'native/metal/Metal.mm';text=source.read_text()
helpers=r'''
#include <mach/mach_time.h>
static double hpNow() {
  static const auto scale=[] { mach_timebase_info_data_t t; mach_timebase_info(&t); return double(t.numer)/t.denom; }();
  return mach_absolute_time()*scale;
}
static std::atomic<uint64_t> hpCounters[16]{};
struct HpScope { unsigned slot; double start=hpNow(); ~HpScope(){hpCounters[slot]+=uint64_t(hpNow()-start);} };
extern "C" uint64_t hpValue(unsigned i) { return hpCounters[i].load(); }
extern "C" void hpReset() { for(auto &v:hpCounters) v=0; }
'''
text=text.replace('namespace kalandra::runtime::metal {',helpers+'\nnamespace kalandra::runtime::metal {',1)
def inject(needle,code):
 global text
 assert text.count(needle)==1,(needle,text.count(needle))
 text=text.replace(needle,needle+code)
inject('bool staging, bool shared = false) {','\n HpScope timer{0};')
inject('~MetalAllocation() {','\n HpScope timer{1};')
inject('static id<MTLCommandBuffer> numericCommandBuffer(Frame &f, std::uint32_t index) {','\n HpScope timer{2};')
inject('static std::expected<void, Error> prepareProgression(Task &task, Frame &f) {','\n HpScope timer{3};')
inject('static void resume(Task &task) {','\n HpScope timer{4};')
inject('static void submitProgression(Frame &f, ExternalCompletion completion, std::size_t index) {','\n const double hpSubmit=hpNow(); HpScope timer{5};')
needle='[command addCompletedHandler:^(id<MTLCommandBuffer> finished) {'
start=text.index('static void submitProgression(');index=text.index(needle,start)+len(needle)
text=text[:index]+'''\n const double hpCallback=hpNow();
 hpCounters[6]+=uint64_t(std::max(0.0,finished.GPUStartTime*1e9-hpSubmit));
 hpCounters[7]+=uint64_t(std::max(0.0,hpCallback-finished.GPUEndTime*1e9));
 hpCounters[8]+=uint64_t(std::max(0.0,(finished.GPUEndTime-finished.GPUStartTime)*1e9));
 ++hpCounters[9];
'''+text[index:]
(build/'Metal.mm').write_text(text)
driver=(accept/'Driver.cpp').read_text()
driver='extern "C" unsigned long long hpValue(unsigned);\nextern "C" void hpReset();\n'+driver
driver=driver.replace('auto before=r.stats();','hpReset();auto before=r.stats();')
needle='const double ns=ffn::elapsed(start);'
driver=driver.replace(needle,needle+'''\nstd::printf("{\\\"host_profile\\\":true,\\\"round\\\":%d,\\\"wall_ns\\\":%.0f,\\\"buckets\\\":[",round,ns);for(unsigned q=0;q<10;++q)std::printf("%s%llu",q?",":"",hpValue(q));std::puts("]}");''')
(build/'Driver.cpp').write_text(driver)
obj=build/'metal.o';binary=build/'profile'
p.run([p.CLANG,'-std=c++23','-O2','-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs','-mmacosx-version-min=26.0','-I'+str(p.ROOT/'native/metal'),'-c',build/'Metal.mm','-o',obj])
p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0','-I'+str(accept),'-I'+str(p.ROOT/'native/benchmarks/fair'),build/'Driver.cpp',obj,accept/'task.a','-framework','Foundation','-framework','Metal','-o',binary])
rows=[]
for process in range(5):
 for m,d in ((1,64),(1,512),(16,512),(64,512)):
  out=p.run([binary,bundle,m,d,100,3,0,0,0,1,1])
  observations=[json.loads(x) for x in out.splitlines()]
  timings=[x for x in observations if not x.get('host_profile')]
  assert len(timings)==5 and all(x['checksum']==m*d*4*100 for x in timings)
  rows.append(dict(m=m,d=d,process=process,iterations=100,samples=observations))
  import statistics
  samples=[x for x in observations if x.get('host_profile') and x['round']>=0]
  print(m,d,[round(statistics.median(x['buckets'][i]/100/1000 for x in samples),2) for i in range(10)],flush=True)
p.evidence(args.output,dict(contract=__doc__,buckets=['allocateMetal','destroyMetal','commandCreate','prepareProgression','resume','submitCPU','submitToGPUStart','GPUEndToCallback','GPU','commandCount'],observations=rows),[Path(__file__),source,build/'Metal.mm',build/'Driver.cpp',accept/'TransformerProgram.h'],[binary,obj,accept/'task.a',*bundle.glob('*.metallib')],accept/'kalandra')
