"""Diagnostic completion alternatives at unchanged work/cancellation boundaries.

Prototype only: dispatch worker waits for actual command completion before
checking errors/cancellation and admitting the next chunk. Never a GPU busy spin.
"""
import argparse,json,sys,shutil
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();assert not args.output.exists()
build=args.build.resolve();build.mkdir(parents=True,exist_ok=True)
accept=p.ROOT/'benchmarks/.build/gpu-work-chunks-2026-10-29/acceptance'
bundle=p.ROOT/'benchmarks/.build/gpu-attention-2026-10-29/acceptance/bundle'
source=p.ROOT/'native/metal/Metal.mm';base=source.read_text()
a=base.index('  static void submitProgression(');b=base.index('  static void flushNumeric(',a)
body=r'''  static void submitProgression(Frame &f, ExternalCompletion completion, std::size_t index) {
    auto *frame=&f;
    dispatch_async(HP_QUEUE, ^{
      auto current=index;
      for (;;) {
        @autoreleasepool {
          auto state=frame->packet.state;
          auto command=frame->progression->checkpoints[current].command;
          const auto ordinal=++state->submissions;
          [command commit];
          [command waitUntilCompleted];
          {
            std::unique_lock lock(state->gateMutex);
            state->gate.wait(lock,[&] {return !state->hold && (!state->holdSubmission || state->holdSubmission!=ordinal);});
          }
          if(state->options.profile && command.GPUEndTime>=command.GPUStartTime)
            state->gpuNanoseconds+=std::uint64_t((command.GPUEndTime-command.GPUStartTime)*1e9);
          const auto succeeded=command.status==MTLCommandBufferStatusCompleted && !state->failCompletion.exchange(false);
          ++state->completions;
          auto &progress=*frame->progression;
          if(succeeded && current+1<progress.checkpoints.size() && !completion.cancelled() && !progressionFault(progress,progress.checkpoints[current].parameters)) {
            ++current; continue;
          }
          frame->succeeded=succeeded;frame->active=false;--state->inFlight;
          completion.signal();return;
        }
      }
    });
  }
'''
variants={
 'global-default':body.replace('HP_QUEUE','dispatch_get_global_queue(QOS_CLASS_DEFAULT,0)'),
 'global-interactive':body.replace('HP_QUEUE','dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE,0)'),
 'serial-initiated':body.replace('HP_QUEUE','f.packet.state->hpQueue'),
}
binaries={'baseline':accept/'gpu'};sources=[Path(__file__),source,accept/'Driver.cpp',accept/'TransformerProgram.h']
for version,new in variants.items():
 text=base[:a]+new+base[b:]
 if version=='serial-initiated':text=text.replace('struct State {','struct State {\n dispatch_queue_t hpQueue=dispatch_queue_create("kalandra.prototype",dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,QOS_CLASS_USER_INITIATED,0));',1)
 f=build/(version+'.mm');f.write_text(text);sources.append(f)
 obj=build/(version+'.o');binary=build/version
 p.run([p.CLANG,'-std=c++23','-O2','-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs','-mmacosx-version-min=26.0','-I'+str(p.ROOT/'native/metal'),'-c',f,'-o',obj])
 p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0','-I'+str(accept),'-I'+str(p.ROOT/'native/benchmarks/fair'),accept/'Driver.cpp',obj,accept/'task.a','-framework','Foundation','-framework','Metal','-o',binary])
 binaries[version]=binary
# Actual outputs, not just the timed shape checksum.
checks=[]
for m,d in ((1,64),(1,512),(16,512),(64,512),(3,37)):
 expected=p.run([binaries['baseline'],bundle,m,d,1,1,0,1,2,1,1])
 for version,binary in binaries.items():
  output=p.run([binary,bundle,m,d,1,1,0,1,2,1,1]);assert output==expected,(version,m,d)
  checks.append(dict(version=version,m=m,d=d,equal=True))
results={}
for version in variants:
 cells=[(v,m,512) for m in (1,64) for v in ('baseline','current')]
 def invoke(cell,count,rounds):
  v,m,d=cell
  return p.observations(p.run([binaries['baseline' if v=='baseline' else version],bundle,m,d,count,rounds,0,0,0,1,1]),m*d*4,count,rounds)
 results[version]=p.measure_paired(cells,invoke,lambda c:c[1]*c[2]*4,blocks=3,target_ms=30,adaptive=True,max_iterations=5000)
p.evidence(args.output,dict(contract=__doc__,checks=checks,variants=results),sources,list(binaries.values())+[accept/'task.a',*bundle.glob('*.metallib')],accept/'kalandra')
