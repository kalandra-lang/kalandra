"""GPU encoder timestamps from an isolated instrumented copy of the real runtime.

Sampling and clock correlation perturb execution. This is a stage diagnostic,
not acceptance wall timing. Production sources and shader libraries are unchanged.
"""
import argparse,io,json,os,shutil,statistics,struct,subprocess,sys,tarfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'));import protocol as p
BASE='f28a71f0d0d4bc11c806b638e8404b46b4451456'
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline',type=Path,required=True);parser.add_argument('--bundle',type=Path,required=True)
parser.add_argument('--build-dir',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--processes',type=int,default=3)
parser.add_argument('--register-policy',choices=['baseline','wide'],default='baseline')
args=parser.parse_args();assert not args.output.exists()
baseline=args.baseline.resolve();bundle=args.bundle.resolve();build=args.build_dir.resolve();build.mkdir(parents=True,exist_ok=False)
for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):assert os.getenv(flag) in (None,'','0'),flag
manifest=json.loads((baseline/'manifest.json').read_text())
for name,record in manifest.items():assert p.sha(baseline/name)==record['sha256'],name
source=build/'source';source.mkdir()
with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(['git','archive',BASE,'native'],cwd=p.ROOT))) as archive:archive.extractall(source,filter='data')
selection=source/'native/metal/MatmulLayout.h'
if args.register_policy=='wide':
    original=selection.read_text();needle='std::uint64_t(rows) * columns >= 256u * 256u;';assert original.count(needle)==1
    selection.write_text(original.replace(needle,'std::uint64_t(rows) * columns >= 8192u;'))
metal=source/'native/metal/Metal.mm';text=metal.read_text()
text=text.replace('#include <vector>','#include <vector>\n#include <cstdio>\n#include <cstdlib>\n#include <mach/mach_time.h>',1)
text=text.replace('  struct Frame {','''  struct TraceStage { std::size_t pc; int kind,transform; std::uint64_t m,n,k; int variant=0; };
  struct Frame {
    id<MTLCounterSampleBuffer> traceBuffer=nil;
    MTLTimestamp traceCpu0=0,traceGpu0=0;
    double traceCpuScale=0;
    std::vector<TraceStage> trace;
''',1)
helper=Path(__file__).parent/'fair/DispatchInstrumentation.inc'
text=text.replace('  static id<MTLCommandBuffer> numericCommandBuffer(',helper.read_text()+'\n  static id<MTLCommandBuffer> numericCommandBuffer(',1)
# The same real encoders, grid selection, shaders, and ownership are retained.
text=text.replace('[command computeCommandEncoder]','traceEncoder(f, command)')
# Undo the helper's intentional ordinary-encoder fallback.
text=text.replace('if (!f.packet.numericProgram) return traceEncoder(f, command);','if (!f.packet.numericProgram) return [command computeCommandEncoder];',1)
text=text.replace('            if (f.numericAcknowledging) {','            if (f.numericAcknowledging) {\n              dumpTrace(f);',1)
needle='''            if (state->options.profile)
              ++state->dispatches;
            [encoder dispatchThreadgroups:'''
replacement='''            if (!f.trace.empty()) f.trace.back().variant = hardwareSkinny?5:hardwareRegister?3:hardwareWide?2:hardwareTile?1:0;
'''+needle
assert needle in text;text=text.replace(needle,replacement,1);metal.write_text(text)
flags=['-std=c++23','-O2','-pthread','-mmacosx-version-min=26.0']
p.run([p.CLANG,*flags,'-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs','-c',metal,'-o',build/'Metal.o'])
p.run(['ar','rcs',build/'metal.a',build/'Metal.o'])
cpp=build/'Driver.cpp';text=(baseline/'Driver.cpp').read_text().replace('../../metal/Metal.h',str(source/'native/metal/Metal.h')).replace('m::Runtime::create()','m::Runtime::create({.profile=true})');cpp.write_text(text)
shutil.copy2(baseline/'Program.h',build/'TransformerProgram.h')
p.run([p.CLANG,*flags,'-ffp-contract=off','-I'+str(build),'-I'+str(source/'native/benchmarks/fair'),cpp,build/'metal.a',baseline/'task.a','-framework','Foundation','-framework','Metal','-o',build/'gpu'])
def invoke(binary,m,d,dump,pattern=0):
    command=list(map(str,[binary,bundle,m,d,1,3,0,dump,pattern]));p.COMMANDS.append(command)
    result=subprocess.run(command,capture_output=True,text=True,timeout=90,check=True)
    traces=[json.loads(line.removeprefix('DISPATCH_PROFILE ')) for line in result.stderr.splitlines() if line.startswith('DISPATCH_PROFILE ')]
    assert all(line.startswith('DISPATCH_PROFILE ') for line in result.stderr.splitlines()),result.stderr
    return (json.loads(result.stdout) if dump else p.observations(result.stdout,m*d*4,1,3)),traces
verification=[];rows=[]
shapes=[(m,d) for d in (64,256,512) for m in (1,16,64)]
for m,d in [(2,3),(3,7),(5,17),(17,33)]+shapes:
    a,_=invoke(baseline/'gpu',m,d,1,1);b,traces=invoke(build/'gpu',m,d,1,1)
    assert struct.pack('<'+'f'*len(a['cells']),*a['cells'])==struct.pack('<'+'f'*len(b['cells']),*b['cells'])
    assert len(traces)==1 and len(traces[0]['stages'])==14
    verification.append(dict(m=m,d=d,cells=m*d,bit_identical=True))
for m,d in shapes:
    for process in range(args.processes):
        samples,traces=invoke(build/'gpu',m,d,0);assert len(samples)==len(traces)==5
        assert all(len(t['stages'])==14 for t in traces)
        rows.append(dict(m=m,d=d,process=process,samples=samples,traces=traces))
    trace=rows[-1]['traces'][-1];print(m,d,[(r['pc'],round(r['ns']/1000,2)) for r in trace['stages']],flush=True)
p.evidence(args.output,dict(contract=__doc__,base_commit=BASE,register_policy=args.register_policy,clock_conversion='Two CPU/GPU timestamp pairs bracket each invocation; CPU timestamp units identified against mach_absolute_time and mach_timebase_info rather than assumed. Raw GPU samples and correlation retained.',verification=verification,observations=rows),[Path(__file__),helper,selection,metal,cpp,baseline/'manifest.json'],[build/'gpu',build/'metal.a',baseline/'gpu',baseline/'metal.a',baseline/'task.a',*bundle.glob('*.metallib')],baseline/'compiler')
