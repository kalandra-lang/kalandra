"""Qualify native synchronous subgraphs inside production task entries."""
import argparse, os, subprocess, sys
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--clang',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
root=Path(__file__).resolve().parents[2]
a.output.mkdir(parents=True,exist_ok=True)
windows=sys.platform=='win32'
host_flags=['-fms-runtime-lib=static'] if windows else ['-pthread']
env=dict(os.environ,KALANDRA_WORKERS='1')
if windows:
    resources=list(a.clang.resolve().parent.parent.glob('lib/clang/*/lib/windows'))
    env['PATH']=os.pathsep.join(map(str,resources))+os.pathsep+env.get('PATH','')
def executable(name): return a.output/(name+('.exe' if windows else ''))
def runtime_file(name):
    return a.compiler.parent/(('' if windows else 'lib')+'kalandra-'+name+('.lib' if windows else '.a'))
local=a.output/'local-closure.k'
local.write_text('''fn make(offset:int)->fn(int)->int:
  fn(value:int): (value*13+offset)%1000003
fn walk(f:fn(int)->int,value:int,rounds:int)->int:
  if rounds==0:value
  else:walk(f,f(value),rounds-1)
fn probe(seed:int,rounds:int)->int:
  walk(make(seed+7),seed,rounds)
fn main()->int:
  probe(17,16)
''')
spin=a.output/'spin.k'
spin.write_text('''fn spin(values:[int],done:Bool)->int:
  if done:values[0]
  else:spin(values,done)
fn main()->int:
  spin([42],true)
''')
def run(args):
    result=subprocess.run(list(map(str,args)),text=True,capture_output=True,timeout=180,
                          env=env)
    assert result.returncode==0,(args,result.returncode,result.stdout,result.stderr)
    print(result.stdout,end='',flush=True)
    return result.stdout
for mode,extra,flags,runtime in [
    ('debug',[],['-O0'],'native-runtime'),
    ('optimized',['-O2'],['-O2'],'task-core-optimized'),
    ('sanitized',['--sanitize'],['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'],'native-runtime-sanitized'),
    ('thread-sanitized',['--thread-sanitize'],['-O1','-g','-fsanitize=thread'],'native-runtime-thread-sanitized')]:
    if windows and mode=='thread-sanitized': continue
    ir=a.output/(mode+'.ll')
    run([a.compiler,root/'native/tests/fixtures/task-native-helpers.k','--test-entry','worker',
         '--emit-llvm',ir,'-o',a.output/(mode+'-cli'),*extra])
    # Pure helpers must not acquire the old synchronous continuation call ABI.
    assert 'kalandra_callable_plan_' not in ir.read_text()
    binary=executable(mode+'-runtime')
    run([a.clang,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-override-module',*host_flags,
         *flags,root/'native/tests/TaskNativeHelpersRuntimeTests.cpp',ir,
         *([root/'native/runtime/TaskABI.cpp'] if runtime=='task-core-optimized' else []),
         runtime_file(runtime),'-o',binary])
    print(mode,flush=True)
    run([binary])
    local_ir=a.output/(mode+'-local.ll')
    run([a.compiler,local,'--test-entry','probe','--emit-llvm',local_ir,
         '-o',a.output/(mode+'-local-cli'),*extra])
    local_binary=executable(mode+'-local-runtime')
    run([a.clang,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-override-module',*host_flags,
         *flags,root/'native/tests/LocalClosureRuntimeTests.cpp',local_ir,
         runtime_file(runtime),'-o',local_binary])
    run([local_binary])
    array_ir=a.output/(mode+'-array.ll')
    run([a.compiler,root/'native/benchmarks/fair/RuntimeWorkloads.k','--test-entry','arrays',
         '--emit-llvm',array_ir,'-o',a.output/(mode+'-array-cli'),*extra])
    # Instrument only allocations in this correctness executable. The wrapper
    # calls the production allocator and records live backing count; acceptance
    # benchmarks use the original, uninstrumented compiler output.
    array_ir.write_text(array_ir.read_text().replace('@kalandra_alloc_v1','@test_alloc'))
    array_binary=executable(mode+'-array-runtime')
    run([a.clang,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-override-module',*host_flags,
         *flags,root/'native/tests/ArrayTailRuntimeTests.cpp',array_ir,
         runtime_file(runtime),'-o',array_binary])
    run([array_binary])
    spin_ir=a.output/(mode+'-spin.ll')
    run([a.compiler,spin,'--test-entry','spin','--emit-llvm',spin_ir,
         '-o',executable(mode+'-spin-cli'),*extra])
    spin_binary=executable(mode+'-spin-runtime')
    run([a.clang,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-override-module',*host_flags,
         *flags,root/'native/tests/ArrayTailSpinTests.cpp',spin_ir,runtime_file(runtime),'-o',spin_binary])
    run([spin_binary])
    for numeric in ('float32','float64'):
        floating=a.output/(mode+'-'+numeric+'-bits.k')
        floating.write_text(f'''fn walk(values:[{numeric}],rounds:int)->{numeric}:
  if rounds==0:values[0]
  else:walk(map(values,fn(value:{numeric}): -value),rounds-1)
fn main()->{numeric}:
  walk([{numeric}(1)],1)
''')
        for policy in (('hardware','software') if mode=='optimized' else ('hardware',)):
            ir=a.output/(mode+'-'+numeric+'-'+policy+'-bits.ll')
            run([a.compiler,floating,'--test-entry','walk','--arithmetic',policy,'--emit-llvm',ir,
                 '-o',executable(mode+'-'+numeric+'-'+policy+'-bits-cli'),*extra])
            binary=executable(mode+'-'+numeric+'-'+policy+'-bits-runtime')
            run([a.clang,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-override-module',*host_flags,
                 *flags,*(['-DFLOAT32'] if numeric=='float32' else []),
                 root/'native/tests/ArrayTailFloatTests.cpp',ir,runtime_file(runtime),'-o',binary])
            run([binary])
    boolean=a.output/(mode+'-array-bool.k')
    boolean.write_text('''fn walk(values:[int],rounds:int)->Bool:
  if rounds==0:values[0]==23
  else:walk(map(values,fn(value:int): value+1),rounds-1)
fn main()->Bool:
  if walk([3],20):
    if walk([23],0):
      if walk([3],0):false
      else:true
    else:false
  else:false
''')
    binary=executable(mode+'-array-bool')
    run([a.compiler,boolean,'-o',binary,*extra])
    assert run([binary])=='true\n'
    for numeric in ('int8','uint8','int16','uint16','int32','uint32','int64','uint64','float32','float64'):
        typed=a.output/(mode+'-'+numeric+'.k')
        typed.write_text(f'''fn walk(values:[{numeric}],rounds:int)->{numeric}:
  if rounds==0:values[0]
  else:walk(map(values,fn(value:{numeric}): value+{numeric}(1)),rounds-1)
fn main()->{numeric}:
  original=[{numeric}(3),{numeric}(4)]
  result=walk(original,20)
  result+original[0]
''')
        for policy in (('hardware','software') if numeric.startswith('float') else ('hardware',)):
            binary=executable(mode+'-'+numeric+'-'+policy)
            run([a.compiler,typed,'--arithmetic',policy,'-o',binary,*extra])
            assert run([binary])=='26\n'

# This source-valid body exceeds the native subgraph's conservative physical
# frame admission. It must retain the pre-existing continuation implementation.
source=a.output/'large-helper.k'
lines=['fn large(x:int)->int:','  value0=0']
lines += [f'  value{i}=value{i-1}+x/(x+{i})' for i in range(1,241)]
lines += ['  value240','fn main()->int ! TaskError:','  try parallel:',
          '    try await spawn large(1)']
source.write_text('\n'.join(lines)+'\n')
binary=executable('large-helper')
run([a.compiler,source,'-O2','-o',binary])
result=subprocess.run([str(binary)],text=True,capture_output=True,timeout=30,
                      env=env)
assert result.returncode==0 and result.stdout=='0\n' and not result.stderr,result
print('Large native subgraph falls back to checked continuation execution',flush=True)
