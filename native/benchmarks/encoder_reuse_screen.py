"""Screen compute encoder reuse within unchanged work-bounded commands.

Variants keep serial dispatch, with either explicit buffer barriers or the
serial encoder's default ordering. This is a prototype, not qualification.
"""
import argparse,json,sys,os
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
a=parser.parse_args();assert not a.output.exists()
build=a.build.resolve();build.mkdir(parents=True,exist_ok=True)
accept=p.ROOT/'benchmarks/.build/gpu-host-costs-2026-10-29/acceptance'
bundle=p.ROOT/'benchmarks/.build/gpu-attention-2026-10-29/acceptance/bundle'
source=p.ROOT/'native/metal/Metal.mm';base=source.read_text()
text=base.replace('id<MTLCommandBuffer> pendingCommand = nil;', '''id<MTLCommandBuffer> pendingCommand = nil;
    id<MTLComputeCommandEncoder> reusedEncoder = nil;
    id<MTLCommandBuffer> encodingCommand = nil;''',1)
text=text.replace('~Frame() {','~Frame() {\n [reusedEncoder endEncoding]; reusedEncoder=nil; encodingCommand=nil;',1)
helper='''  static void endReusedEncoder(Frame &f) {
    [f.reusedEncoder endEncoding];f.reusedEncoder=nil;f.encodingCommand=nil;
  }
  static id<MTLComputeCommandEncoder> beginNumericEncoder(Frame &f,id<MTLCommandBuffer> command) {
    if(!f.progression || !f.progression->workChunks) return [command computeCommandEncoder];
    if(f.reusedEncoder) {
      if(f.encodingCommand!=command) std::abort();
      HP_BARRIER
      return f.reusedEncoder;
    }
    f.reusedEncoder=[command computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
    if(f.reusedEncoder) f.encodingCommand=command;
    return f.reusedEncoder;
  }
  static void finishNumericEncoder(Frame &f,id<MTLComputeCommandEncoder> encoder) {
    if(encoder!=f.reusedEncoder) [encoder endEncoding];
  }
'''
idx=text.index('  static void retainProgressionChunk(');text=text[:idx]+helper+text[idx:]
text=text.replace('static void checkpointProgression(Frame &f) {','static void checkpointProgression(Frame &f) {\n endReusedEncoder(f);',1)
# Change only prepared transform and ordinary numeric encoding; reduction and
# blit encoders retain their original ownership and dispatch behavior.
start=text.index('  static id<MTLCommandBuffer> tensorTransformCommand(');end=text.index('  static void submitCommand(',start)
region=text[start:end].replace('[command computeCommandEncoder]','beginNumericEncoder(f,command)').replace('[encoder endEncoding]','finishNumericEncoder(f,encoder)')
text=text[:start]+region+text[end:]
start=text.index('            id<MTLComputeCommandEncoder> encoder =',text.index('  static void resume('))
end=text.index('            if ((f.packet.graph && f.graphPhase == 2) || f.packet.numericScalar)',start)
region=text[start:end].replace('[command computeCommandEncoder]','beginNumericEncoder(f,command)').replace('[encoder endEncoding]','finishNumericEncoder(f,encoder)')
text=text[:start]+region+text[end:]
variants={'barrier':'[f.reusedEncoder memoryBarrierWithScope:MTLBarrierScopeBuffers];','serial':''}
binaries={'baseline':accept/'gpu'};sources=[Path(__file__),source,accept/'Driver.cpp',accept/'TransformerProgram.h']
for variant,barrier in variants.items():
 f=build/(variant+'.mm');f.write_text(text.replace('HP_BARRIER',barrier));sources.append(f)
 obj=build/(variant+'.o');binary=build/variant
 p.run([p.CLANG,'-std=c++23','-O2','-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs','-mmacosx-version-min=26.0','-I'+str(p.ROOT/'native/metal'),'-c',f,'-o',obj])
 p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0','-I'+str(accept),'-I'+str(p.ROOT/'native/benchmarks/fair'),accept/'Driver.cpp',obj,accept/'task.a','-framework','Foundation','-framework','Metal','-o',binary])
 binaries[variant]=binary
checks=[]
for m,d in ((1,64),(1,512),(16,512),(64,512),(3,37),(17,33)):
 for mode in range(3):
  for pattern in (0,2):
   command=[bundle,m,d,1,1,mode,1,pattern,1,1,1]
   expected=p.run([binaries['baseline'],*command])
   for version in variants:
    env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')
    actual=p.run([binaries[version],*command],env=env)
    assert actual==expected,(m,d,mode,pattern,version)
    checks.append(dict(m=m,d=d,mode=mode,pattern=pattern,version=version,equal=True))
results={}
for version in variants:
 cells=[(v,m,d,mode) for m,d in ((1,64),(1,512),(16,512),(64,512)) for mode in (0,2) for v in ('baseline','current')]
 def invoke(cell,count,rounds):
  v,m,d,mode=cell
  samples=p.observations(p.run([binaries['baseline' if v=='baseline' else version],bundle,m,d,count,rounds,mode,0,0,1,1,1]),m*d*4,count,rounds)
  commands=5 if m==64 else 3 if m==16 else 1
  assert all(s['submissions']==commands*count for s in samples)
  return samples
 results[version]=p.measure_paired(cells,invoke,lambda c:c[1]*c[2]*4,blocks=3,target_ms=30,adaptive=True,max_iterations=5000)
p.evidence(a.output,dict(contract=__doc__,checks=checks,variants=results),sources,list(binaries.values())+[accept/'task.a',*bundle.glob('*.metallib')],accept/'kalandra')
