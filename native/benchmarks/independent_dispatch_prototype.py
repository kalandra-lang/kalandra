"""Build a copied-runtime independent-dispatch prototype; production is unchanged."""
import argparse,json,shutil,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--build',type=Path,required=True);a=parser.parse_args()
build=a.build.resolve();build.mkdir(parents=True,exist_ok=False)
fair=Path(__file__).parent/'fair';source=p.ROOT/'native/metal/Metal.mm';text=source.read_text()
def replace(old,new):
 global text
 assert text.count(old)==1,(old,text.count(old));text=text.replace(old,new)
replace('#include <cstring>','#include <cstring>\n#include <cstdlib>\n#include <cstdio>')
replace('    id<MTLCommandBuffer> pendingCommand = nil;','    id<MTLCommandBuffer> pendingCommand = nil;\n    id<MTLComputeCommandEncoder> independentEncoder = nil;\n    std::size_t independentEnd = SIZE_MAX;')
replace('    ~Frame() {','    ~Frame() {\n      if (independentEncoder) [independentEncoder endEncoding];')
replace('  static bool copy(Invocation &', (fair/'IndependentDispatch.inc').read_text()+'\n  static bool copy(Invocation &')
replace('  static void submitCommand(Task &task, Frame &f, id<MTLCommandBuffer> command) {','  static void submitCommand(Task &task, Frame &f, id<MTLCommandBuffer> command) {\n    closeIndependentEncoder(f);')
replace('  static void fail(Task &task, Frame &f, std::int64_t error) {','  static void fail(Task &task, Frame &f, std::int64_t error) {\n    closeIndependentEncoder(f);')
replace('            f.numericPostWanted = numericPostCandidate(f);','            planIndependentGroup(f);\n            f.numericPostWanted = f.independentEnd == SIZE_MAX && numericPostCandidate(f);')
replace('''            id<MTLComputeCommandEncoder> encoder =
                state->permit() ? [command computeCommandEncoder] : nil;''','''            id<MTLComputeCommandEncoder> encoder =
                state->permit() ? independentComputeEncoder(f, command) : nil;''')
replace('''            [encoder endEncoding];
            if (fusePost) {''','''            finishIndependentDispatch(f, encoder);
            if (fusePost) {''')
(build/'Metal.mm').write_text(text)
program=p.ROOT/'benchmarks/.build/gpu-closed-entry-2026-10-29/acceptance/TransformerProgram.h'
shutil.copy2(program,build/'TransformerProgram.h')
task=p.ROOT/'build/native/native/libkalandra-task-core-optimized.a';flags=['-std=c++23','-O2','-mmacosx-version-min=26.0']
p.run([p.CLANG,*flags,'-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs','-I'+str(p.ROOT/'native/metal'),'-c',build/'Metal.mm','-o',build/'metal.o'])
p.run([p.CLANG,*flags,'-pthread','-ffp-contract=off','-I'+str(build),'-I'+str(p.ROOT/'native/metal'),'-I'+str(fair),fair/'TransformerClosedMetal.cpp',build/'metal.o',task,'-framework','Foundation','-framework','Metal','-o',build/'gpu'])
sources=[Path(__file__),fair/'IndependentDispatch.inc',source,p.ROOT/'native/metal/Metal.h',fair/'TransformerClosedMetal.cpp',fair/'TransformerBlock.h',program,task]
artifacts=[build/'Metal.mm',build/'TransformerProgram.h',build/'metal.o',build/'gpu']
(build/'manifest.json').write_text(json.dumps(dict(signature={p.key(f.resolve()):p.sha(f) for f in sources},artifacts={p.key(f):p.sha(f) for f in artifacts},commands=p.COMMANDS),indent=2)+'\n')
print(build/'gpu')
