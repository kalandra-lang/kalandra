"""Process-level parity of direct failure reporting and recoverable boundaries."""
import argparse,json,os,subprocess,sys
from pathlib import Path
from OrdinaryTestEnvironment import configure, qualified_variants
from ParallelTestRunner import run_parallel

q=argparse.ArgumentParser();q.add_argument('--compiler',type=Path,required=True);q.add_argument('--clang',type=Path,required=True);q.add_argument('--output',type=Path,required=True);a=q.parse_args();a.output.mkdir(parents=True,exist_ok=True)
configure(a.clang)
root=Path(__file__).resolve().parents[2]
windows=sys.platform=='win32'
suffix='.exe' if windows else ''
obj_ext='.obj' if windows else '.o'
host_flags=['-fms-runtime-lib=static'] if windows else ['-pthread']
def run(args):
 r=subprocess.run(list(map(str,args)),capture_output=True,text=True)
 if r.returncode:raise RuntimeError(str(args)+'\n'+r.stdout+r.stderr)
 return r.stdout
modes=[('debug',[],['-O0'],'native-runtime'),('optimized',['-O2'],['-O2'],'task-core-optimized'),('sanitized',['-O2','--sanitize'],['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'],'native-runtime-sanitized'),('thread-sanitized',['-O2','--thread-sanitize'],['-O1','-g','-fsanitize=thread'],'native-runtime-thread-sanitized')]
modes=qualified_variants(modes,a.compiler)
if windows:modes=[mode for mode in modes if mode[0]!='thread-sanitized']
values=[(0,1),(17,3),(2**63-1,0),(-2**63,0),(2**63-1,1),(-2**63,-1),(-2**63,1),(0,0),(17,-1),(-17,3),(2**63-1,2**63-1)]

DRIVER_TEMPLATE=r'''
#include "FAULT"
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <type_traits>
using I=std::int64_t;using T=TYPE;
extern "C" T _mlir_ciface_kalandra_test_entry_ordinary_direct_v1(I,I);
extern "C" T _mlir_ciface_kalandra_test_entry_ordinary_v1(I,I,kalandra::runtime::OrdinaryFault*);
extern "C" std::int32_t kalandra_ordinary_finish_v1(std::uint64_t,std::int32_t,const kalandra::runtime::OrdinaryFault*) noexcept;
template<class V> std::uint64_t bits(V value){
 if constexpr(std::is_floating_point_v<V>){using U=std::conditional_t<sizeof(V)==4,std::uint32_t,std::uint64_t>;return std::bit_cast<U>(value);}
 else return static_cast<std::uint64_t>(value);
}
int main(int argc,char **argv){
 if(argc!=4)return 2;I a=std::strtoll(argv[1],nullptr,10),b=std::strtoll(argv[2],nullptr,10);
 kalandra::runtime::OrdinaryFault fault{};
 auto value=(std::string_view(argv[3])=="direct")
   ? _mlir_ciface_kalandra_test_entry_ordinary_direct_v1(a,b)
   : _mlir_ciface_kalandra_test_entry_ordinary_v1(a,b,&fault);
 return kalandra_ordinary_finish_v1(bits(value),KIND,&fault);
}
'''

TYPES=[('i64','std::int64_t',5),('f32','float',0),('f64','double',1),('bool','bool',2)]

def compile_driver(item):
 mode,_,flags,_,lang,cpp,kind=item
 driver=a.output/(f'driver-{mode}-{lang}.cpp')
 obj=a.output/(f'driver-{mode}-{lang}{obj_ext}')
 driver.write_text(DRIVER_TEMPLATE.replace('FAULT',str(root/'native/runtime/OrdinaryFault.h')).replace('TYPE',cpp).replace('KIND',str(kind)))
 run([a.clang,'-std=c++23',*flags,'-ffp-contract=off',*host_flags,'-c',driver,'-o',obj])
 return (mode,lang),obj

driver_objs=dict(run_parallel(
 [(mode,extra,flags,library,lang,cpp,kind) for mode,extra,flags,library in modes for lang,cpp,kind in TYPES],
 compile_driver))

def run_op_task(task):
 mode,extra,flags,library,lang,op,label=task
 name=f'{mode}-{lang}-{label}';src=a.output/(name+'.k');ir=a.output/(name+'.ll')
 expression=f'(a{op}b)>0' if lang=='bool' else f'{lang}(a{op}b)'
 src.write_text(f'fn value(a:i64,b:i64)->{lang}:\n  return {expression}\nfn main()->{lang}:\n  return {"false" if lang=="bool" else lang+"(0)"}\n')
 run([a.compiler,src,'--test-entry','value','--emit-llvm',ir,'--no-link',*extra])
 assert '_mlir_ciface_kalandra_test_entry_ordinary_direct_v1(' in ir.read_text(),name
 runtime=a.compiler.parent/(('' if windows else 'lib')+'kalandra-'+library+('.lib' if windows else '.a'))
 exe=a.output/(name+'-harness'+suffix)
 run([a.clang,*flags,'-ffp-contract=off','-Wno-override-module',*host_flags,driver_objs[(mode,lang)],ir,runtime,'-o',exe])
 chunk=[]
 env=dict(os.environ,KALANDRA_TEST_REPORT_CLEANUP='1')
 for x,y in values:
  records=[subprocess.run([str(exe),str(x),str(y),contract],env=env,capture_output=True,text=True) for contract in ('recoverable','direct')]
  assert (records[0].returncode,records[0].stdout,records[0].stderr)==(records[1].returncode,records[1].stdout,records[1].stderr),(name,x,y,[(r.returncode,r.stdout,r.stderr) for r in records])
  if records[0].returncode:assert 'cleanup: live=0 allocations=0 releases=0' in records[0].stderr
  chunk.append(dict(mode=mode,type=lang,operator=label,args=[x,y],status=records[0].returncode,stdout=records[0].stdout,stderr=records[0].stderr))
 print(name,'11 direct/recoverable process outcomes match',flush=True)
 return chunk

op_tasks=[(mode,extra,flags,library,lang,op,label)
          for mode,extra,flags,library in modes
          for lang,_,_ in TYPES
          for op,label in [('+','add'),('/','divide'),('%','remainder')]]
results=[record for chunk in run_parallel(op_tasks, run_op_task) for record in chunk]
(a.output/'results.json').write_text(json.dumps(results,indent=2)+'\n')

# Exercise the compiler-generated main, not only hand-written host adapters.
def run_cli_task(task):
 mode,extra,lang,label,inputs=task
 name=f'{mode}-{lang}-{label}'
 text=(a.output/(name+'.k')).read_text().split('fn main()->')[0]+f'fn main()->{lang}:\n  return value({inputs[0]},{inputs[1]})\n'
 source=a.output/(name+'-cli.k');exe=a.output/(name+'-cli'+suffix);source.write_text(text)
 run([a.compiler,source,*extra,'-o',exe])
 env=dict(os.environ,KALANDRA_TEST_REPORT_CLEANUP='1')
 actual=subprocess.run([exe],capture_output=True,text=True,env=env)
 expected=subprocess.run([str(a.output/(name+'-harness'+suffix)),*map(str,inputs),'recoverable'],capture_output=True,text=True,env=env)
 assert (actual.returncode,actual.stdout,actual.stderr)==(expected.returncode,expected.stdout,expected.stderr),(name,actual.stderr,expected.stderr)
 print(name,'standalone failure matches',flush=True)
 return dict(name=name,status=actual.returncode,stderr=actual.stderr)

cli_tasks=[(mode,extra,lang,label,inputs)
           for mode,extra,_,_ in modes
           for lang in ['i64','f32','f64','bool']
           for label,inputs in [('add',(2**63-1,1)),('divide',(17,0)),('remainder',(17,0))]]
cli=run_parallel(cli_tasks, run_cli_task)
(a.output/'cli-results.json').write_text(json.dumps(cli,indent=2)+'\n')
