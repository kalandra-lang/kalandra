"""Deterministic CPU-builder/GPU-callback lifetime gates, without shipping test hooks."""
import argparse, hashlib, json, subprocess, sys
from pathlib import Path
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'native/benchmarks/fair'))
import prepared_program
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler',type=Path,required=True)
parser.add_argument('--clang',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
a=parser.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
compiler=a.compiler.resolve();native=compiler.parent;commands=[]
def run(command):
 command=list(map(str,command));commands.append(command)
 result=subprocess.run(command,capture_output=True,text=True,timeout=360)
 assert result.returncode==0,(command,result.returncode,result.stdout,result.stderr)
 assert not any(x in result.stderr for x in ('AddressSanitizer','ThreadSanitizer','runtime error:')),result.stderr
 return result.stdout+result.stderr
model=out/'model.k'
model.write_text((root/'native/benchmarks/TransformerBlock.k').read_text().split('fn main(')[0]+'''fn main()->Tensor[float32,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    x:Tensor[float32,2,2]=tensor([[1,0],[0,1]])
    up:Tensor[float32,2,8]=tensor([[1,0,1,0,1,0,1,0],[0,1,0,1,0,1,0,1]])
    down:Tensor[float32,8,2]=tensor([[1,0],[0,1],[1,0],[0,1],[1,0],[0,1],[1,0],[0,1]])
    a=try await transfer(x,gpu)
    b=try await transfer(up,gpu)
    c=try await transfer(down,gpu)
    y=try await spawn block(a,a,a,a,a,b,c,0.00001,0.70710677) on gpu
    try await transfer(y,devices.cpu())
''')
bundle=out/'bundle';run([compiler,model,'--target','metal','--arithmetic','hardware','-O2','-o',bundle])
host=(bundle/'host.cpp').read_text()
(out/'TransformerProgram.h').write_text(prepared_program.header(host))
(out/'LegacyProgram.h').write_text(prepared_program.header(host,False))
production=root/'native/metal/Metal.mm';text=production.read_text()
marker='namespace kalandra::runtime::metal {';assert text.count(marker)==1
text=text.replace(marker,'''static std::atomic<bool> klPause{false}, klArrived{false};
static std::atomic<bool> klCallbackPause{false}, klCallbackArrived{false};
extern "C" void klOverlapPause(bool value) {
  if(value) klArrived.store(false);
  klPause.store(value);
}
extern "C" bool klOverlapPaused() { return klArrived.load(); }
extern "C" void klOverlapCallbackPause(bool value) {
  if(value) klCallbackArrived.store(false);
  klCallbackPause.store(value);
}
extern "C" bool klOverlapCallbackPaused() { return klCallbackArrived.load(); }
'''+marker)
start=text.index('  static void checkpointProgression(')
position=text.index('    driveProgression(f);',start)+len('    driveProgression(f);')
text=text[:position]+'''
    if(klPause.load()) {
      klArrived.store(true);
      while(klPause.load()) std::this_thread::yield();
    }'''+text[position:]
needle='        driveProgression(*frame, index, succeeded);';assert text.count(needle)==1
text=text.replace(needle,'''        if(klCallbackPause.load()) {
          klCallbackArrived.store(true);
          while(klCallbackPause.load()) std::this_thread::yield();
        }
'''+needle)
text=text.replace('#include <vector>','#include <vector>\n#include <thread>',1)
source=out/'gated.mm';source.write_text(text)
variants=[('optimized',['-O2'],'libkalandra-task-core-optimized.a'),
 ('debug',['-O0'],'libkalandra-native-runtime.a'),
 ('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'],'libkalandra-native-runtime-sanitized.a'),
 ('thread-sanitized',['-O1','-g','-fsanitize=thread','-fno-omit-frame-pointer'],'libkalandra-native-runtime-thread-sanitized.a')]
records=[]
for mode,flags,task in variants:
 obj=out/(mode+'.o');binary=out/mode
 run([a.clang,'-std=c++23',*flags,'-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs','-mmacosx-version-min=26.0','-I'+str(root/'native/metal'),'-c',source,'-o',obj])
 run([a.clang,'-std=c++23',*flags,'-pthread','-ffp-contract=off','-I'+str(out),root/'native/tests/EncodingOverlapTests.cpp',obj,native/task,'-framework','Foundation','-framework','Metal','-o',binary])
 result=run([binary,bundle]);print(mode+': '+result.strip(),flush=True)
 records.append(dict(mode=mode,result=result))
paths=[Path(__file__),root/'native/tests/EncodingOverlapTests.cpp',production,root/'native/metal/Metal.h',source,compiler]
paths += [out/mode for mode,_,_ in variants]
(out/'qualification.json').write_text(json.dumps(dict(contract=__doc__,records=records,commands=commands,sha256={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}),indent=2)+'\n')
