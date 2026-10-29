"""Draft benchmark-only incremental pre-encoding prototype.

Read a frozen runtime and change only the time at which a complete checkpoint
is submitted. Each subsequent command still waits for predecessor ACK, fault
and cancellation checks. No production file is modified.
"""
from pathlib import Path

def patch(source):
 def replace(old,new,count=1):
  nonlocal source
  assert source.count(old)==count,(old[:90],source.count(old),count)
  source=source.replace(old,new)
 replace('    std::shared_ptr<MetalAllocation> arena;','''    std::mutex progressionMutex;
    ExternalCompletion progressionCompletion;
    bool liveEncoding = false, encodingDone = false, commandRunning = false;
    bool stoppedEncoding = false, encodingSucceeded = true, signaledEncoding = false;
    std::size_t nextCommand = 0;
    std::shared_ptr<MetalAllocation> arena;''')
 # Only the ownership vector and checkpoint list are shared with callbacks.
 needle='''  static void retainProgressionChunk(Frame &f, bool keepWork = false) {'''
 assert needle in source
 at=source.index(needle);body=source.index('auto &p = *f.progression;',at)
 source=source[:body]+source[body:].replace('auto &p = *f.progression;','auto &p = *f.progression;\n    std::lock_guard progressionLock(p.progressionMutex);',1)
 replace('static void checkpointProgression(Frame &f)', 'static void checkpointProgression(Task &task, Frame &f)')
 start=source.index('  static void checkpointProgression(Task &task, Frame &f)');end=source.index('  static bool progressionFault(',start)
 source=source[:start]+'''  static void checkpointProgression(Task &task, Frame &f) {
    retainProgressionChunk(f);
    auto &p = *f.progression;
    {
      std::lock_guard lock(p.progressionMutex);
      p.checkpoints.push_back({f.pendingCommand, p.owners.size()});
      f.pendingCommand = nil;
    }
    if (!p.liveEncoding) {
      p.progressionCompletion = task.suspendExternal();
      if (!p.progressionCompletion) return;
      p.liveEncoding = true;
    }
    driveProgression(f);
  }
'''+source[end:]
 source=source.replace('checkpointProgression(f);','checkpointProgression(task, f);')
 start=source.index('  static void submitProgression(');end=source.index('  static void fail(',start)
 source=source[:start]+'''  static void driveProgression(Frame &f, std::size_t acknowledged = SIZE_MAX, bool succeeded = true) {
    auto &p = *f.progression;
    std::size_t index = 0;
    bool submit = false, signal = false;
    ExternalCompletion completion;
    {
      std::lock_guard lock(p.progressionMutex);
      if (!p.liveEncoding || p.signaledEncoding ||
          (acknowledged == SIZE_MAX && p.commandRunning)) return;
      completion = p.progressionCompletion;
      if (acknowledged != SIZE_MAX) {
        // The callback retains commandRunning until it enters this decision.
        // Clearing it earlier would let the builder signal/free the frame
        // before the callback's final driveProgression access.
        p.commandRunning = false;
        p.encodingSucceeded &= succeeded;
        p.stoppedEncoding |= !succeeded ||
                             progressionFault(p, p.checkpoints[acknowledged].parameters);
      }
      if (completion.cancelled()) p.stoppedEncoding = true;
      if (!p.stoppedEncoding && p.nextCommand < p.checkpoints.size()) {
        index = p.nextCommand++;
        p.commandRunning = true;
        submit = true;
      } else if (p.encodingDone) {
        p.signaledEncoding = true;
        f.succeeded = p.encodingSucceeded;
        f.active = false;
        --f.packet.state->inFlight;
        signal = true;
      }
    }
    if (submit) submitProgression(f, completion, index);
    else if (signal) completion.signal();
    // Neither path may access the frame after handing off or signaling.
  }
  static void submitProgression(Frame &f, ExternalCompletion completion, std::size_t index) {
    auto *frame = &f;
    id<MTLCommandBuffer> command;
    {
      std::lock_guard lock(f.progression->progressionMutex);
      command = f.progression->checkpoints[index].command;
    }
    const auto ordinal = ++f.packet.state->submissions;
    [command addCompletedHandler:^(id<MTLCommandBuffer> finished) {
#if __has_feature(thread_sanitizer)
      __tsan_acquire((__bridge void *)finished);
#endif
      @autoreleasepool {
        auto state = frame->packet.state;
        {
          std::unique_lock lock(state->gateMutex);
          state->gate.wait(lock, [&] {
            return !state->hold && (!state->holdSubmission || state->holdSubmission != ordinal);
          });
        }
        if (state->options.profile && finished.GPUEndTime >= finished.GPUStartTime)
          state->gpuNanoseconds += std::uint64_t((finished.GPUEndTime-finished.GPUStartTime)*1e9);
        const bool succeeded = finished.status == MTLCommandBufferStatusCompleted &&
                               !state->failCompletion.exchange(false);
        ++state->completions;
        driveProgression(*frame, index, succeeded);
      }
    }];
#if __has_feature(thread_sanitizer)
    __tsan_release((__bridge void *)command);
#endif
    [command commit];
  }
  static void finishProgressionEncoding(Frame &f, bool abort) {
    if (!f.progression || !f.progression->liveEncoding) return;
    {
      auto &p = *f.progression;
      std::lock_guard lock(p.progressionMutex);
      if (p.encodingDone) return;
      p.encodingDone = true;
      p.stoppedEncoding |= abort;
      f.numericAcknowledging = true;
      f.started = true;
      f.error = Error::Device;
    }
    driveProgression(f);
  }
  struct ProgressionEncodingExit {
    Frame &frame;
    ~ProgressionEncodingExit() { finishProgressionEncoding(frame, true); }
  };
  static void flushNumeric(Task &task, Frame &f) {
    f.numericAcknowledging = true;
    if (f.progression) {
      if (f.pendingCommand) checkpointProgression(task, f);
      finishProgressionEncoding(f, false);
    } else submitCommand(task, f, f.pendingCommand);
  }
'''+source[end:]
 replace('  static void resume(Task &task) {\n    for (;;) {','  static void resume(Task &task) {\n    ProgressionEncodingExit encodingExit{*static_cast<Frame *>(task.frame())};\n    for (;;) {')
 return source

def single_command_fastpath(source):
 source=source.replace('std::lock_guard progressionLock(p.progressionMutex);',
   'std::unique_lock progressionLock(p.progressionMutex, std::defer_lock);\n    if (p.liveEncoding) progressionLock.lock();')
 old = """    if (f.progression) {
      if (f.pendingCommand) checkpointProgression(task, f);
      finishProgressionEncoding(f, false);"""
 new = """    if (f.progression) {
      if (!f.progression->liveEncoding) {
        retainProgressionChunk(f);
        submitCommand(task, f, f.pendingCommand);
        return;
      }
      if (f.pendingCommand) checkpointProgression(task, f);
      finishProgressionEncoding(f, false);"""
 assert source.count(old)==1
 return source.replace(old,new)

if __name__ == '__main__':
 import argparse,json,sys,os
 sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
 import protocol as p
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--build',type=Path,required=True)
 parser.add_argument('--output',type=Path,required=True)
 parser.add_argument('--single-command-fastpath',action='store_true')
 a=parser.parse_args();assert not a.output.exists()
 build=a.build.resolve();build.mkdir(parents=True,exist_ok=True)
 accept=p.ROOT/'benchmarks/.build/gpu-transpose-view-2026-10-29/acceptance'
 bundle=p.ROOT/'benchmarks/.build/gpu-attention-2026-10-29/acceptance/bundle'
 source=p.ROOT/'native/metal/Metal.mm';generated=build/'overlap.mm'
 text=patch(source.read_text())
 if a.single_command_fastpath:text=single_command_fastpath(text)
 generated.write_text(text)
 obj=build/'overlap.o';binary=build/'overlap'
 p.run([p.CLANG,'-std=c++23','-O2','-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs','-mmacosx-version-min=26.0','-I'+str(p.ROOT/'native/metal'),'-c',generated,'-o',obj])
 p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0','-I'+str(accept),'-I'+str(p.ROOT/'native/benchmarks/fair'),accept/'Driver.cpp',obj,accept/'task.a','-framework','Foundation','-framework','Metal','-o',binary])
 checks=[]
 for m,d in ((1,64),(1,512),(16,512),(64,512),(3,37),(17,33)):
  for mode in range(3):
   for pattern in (0,2):
    args=[bundle,m,d,1,1,mode,1,pattern,1,1,1,1,1]
    expected=p.run([accept/'gpu',*args])
    actual=p.run([binary,*args],env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1'))
    assert actual==expected,(m,d,mode,pattern)
    checks.append(dict(m=m,d=d,mode=mode,pattern=pattern,equal=True))
 print('36 full-output checks passed; starting isolated timing',flush=True)
 cells=[(v,m,d,mode) for m,d in ((1,64),(1,512),(16,512),(64,512)) for mode in (0,2) for v in ('baseline','current')]
 def invoke(cell,count,rounds):
  v,m,d,mode=cell
  samples=p.observations(p.run([accept/'gpu' if v=='baseline' else binary,bundle,m,d,count,rounds,mode,0,0,1,1,1,1,1]),m*d*4,count,rounds)
  commands=5 if m==64 else 3 if m==16 else 1
  assert all(s['submissions']==commands*count for s in samples)
  return samples
 result=p.measure_paired(cells,invoke,lambda c:c[1]*c[2]*4,blocks=3,target_ms=30,adaptive=True,max_iterations=5000)
 p.evidence(a.output,dict(contract=__doc__,single_command_fastpath=a.single_command_fastpath,checks=checks,result=result),[Path(__file__),source,p.ROOT/'native/metal/Metal.h',generated,accept/'Driver.cpp',accept/'TransformerProgram.h'],[accept/'gpu',binary,accept/'task.a',*bundle.glob('*.metallib')],accept/'kalandra')
