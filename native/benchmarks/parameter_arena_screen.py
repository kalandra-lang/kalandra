"""Prototype invocation-local parameter arena; exact old shaders and output checks.

Not production: admission/failure qualification is still required before use.
Slices share a fresh invocation-owned page and preserve distinct writable flags.
"""
import argparse,sys,json
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
a=parser.parse_args();assert not a.output.exists()
build=a.build.resolve();build.mkdir(parents=True,exist_ok=True)
accept=p.ROOT/'benchmarks/.build/gpu-work-chunks-2026-10-29/acceptance'
bundle=p.ROOT/'benchmarks/.build/gpu-attention-2026-10-29/acceptance/bundle'
source=p.ROOT/'native/metal/Metal.mm';text=source.read_text()
text=text.replace('struct MetalAllocation {','''struct MetalAllocation {
  std::shared_ptr<MetalAllocation> parent;
  std::uint64_t offset=0;
  void *contents() const {return static_cast<std::byte *>(buffer.contents)+offset;}
  MetalAllocation(std::shared_ptr<MetalAllocation> p,std::uint64_t n,std::uint64_t at)
    :parent(p),offset(at),state(p->state),buffer(p->buffer),bytes(n),staging(true) {}
''',1)
text=text.replace('~MetalAllocation() {','~MetalAllocation() {\n if(parent) {buffer=nil;return;}',1)
text=text.replace('bool workChunks = false;','bool workChunks = false;\n std::shared_ptr<MetalAllocation> arena;\n std::uint64_t arenaUsed=0;',1)
needle='  static id<MTLCommandBuffer> numericCommandBuffer('
idx=text.index(needle)
text=text[:idx]+'''  static std::expected<std::shared_ptr<MetalAllocation>,Error> hpParameters(Frame &f,std::uint32_t index,std::uint64_t n) {
    auto state=f.packet.state;
    if(!f.progression || !f.progression->workChunks || n>256) return allocateMetal(state,index,n,true);
    auto &p=*f.progression;
    if(!p.arena) {
      auto allocation=allocateMetal(state,index,256*f.packet.numericProgram->code.size(),true);
      if(!allocation)return std::unexpected(allocation.error());
      p.arena=*allocation;
    }
    if(!state->permit())return std::unexpected(Error::Allocation);
    if(p.arenaUsed+256>p.arena->bytes)return std::unexpected(Error::Budget);
    try {
      auto result=std::make_shared<MetalAllocation>(p.arena,n,p.arenaUsed);p.arenaUsed+=256;return result;
    }catch(...){return std::unexpected(Error::Allocation);}
  }
'''+text[idx:]
for old,new in [
 ('allocateMetal(state, kernel.index, 24, true)','hpParameters(f, kernel.index, 24)'),
 ('allocateMetal(state, index, parameterBytes, true)','hpParameters(f, index, parameterBytes)'),
 ('allocateMetal(state, index, 4ull * post->parameterWords(), true)','hpParameters(f, index, 4ull * post->parameterWords())')]:
 assert text.count(old)==1,(old,text.count(old));text=text.replace(old,new)
# All direct CPU parameter reads honor the slice offset; other allocations retain offset zero.
text=text.replace('->buffer.contents','->contents()')
# The helper itself uses the Objective-C object's contents property, unchanged.
for name in ('f.staging','f.fusedParameters'):
 text=text.replace('setBuffer:'+name+'->buffer offset:0','setBuffer:'+name+'->buffer offset:'+name+'->offset')
f=build/'Metal.mm';f.write_text(text);obj=build/'metal.o';binary=build/'arena'
p.run([p.CLANG,'-std=c++23','-O2','-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs','-mmacosx-version-min=26.0','-I'+str(p.ROOT/'native/metal'),'-c',f,'-o',obj])
p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0','-I'+str(accept),'-I'+str(p.ROOT/'native/benchmarks/fair'),accept/'Driver.cpp',obj,accept/'task.a','-framework','Foundation','-framework','Metal','-o',binary])
checks=[]
for m,d in ((1,64),(1,512),(16,512),(64,512),(3,37)):
 for mode in range(3):
  for pattern in (0,2):
   args=[bundle,m,d,1,1,mode,1,pattern,1,1]
   assert p.run([binary,*args])==p.run([accept/'gpu',*args]),(m,d,mode,pattern)
   checks.append(dict(m=m,d=d,mode=mode,pattern=pattern,equal=True))
cells=[(v,m,512,mode) for m in (1,64) for mode in range(3) for v in ('baseline','current')]
def invoke(cell,count,rounds):
 v,m,d,mode=cell
 return p.observations(p.run([accept/'gpu' if v=='baseline' else binary,bundle,m,d,count,rounds,mode,0,0,1,1]),m*d*4,count,rounds)
results=p.measure_paired(cells,invoke,lambda c:c[1]*c[2]*4,blocks=5,target_ms=30,adaptive=True,max_iterations=5000)
results.update(contract=__doc__,checks=checks)
p.evidence(a.output,results,[Path(__file__),source,f,accept/'Driver.cpp',accept/'TransformerProgram.h'],[binary,accept/'gpu',accept/'task.a',*bundle.glob('*.metallib')],accept/'kalandra')
