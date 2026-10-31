"""Process-level parity of direct failure reporting and recoverable boundaries."""
import argparse,json,os,subprocess,sys
from pathlib import Path
from OrdinaryTestEnvironment import configure, qualified_variants
q=argparse.ArgumentParser();q.add_argument('--compiler',type=Path,required=True);q.add_argument('--clang',type=Path,required=True);q.add_argument('--output',type=Path,required=True);a=q.parse_args();a.output.mkdir(parents=True,exist_ok=True)
configure(a.clang)
root=Path(__file__).resolve().parents[2]
windows=sys.platform=='win32'
suffix='.exe' if windows else ''
def run(args):
 r=subprocess.run(list(map(str,args)),capture_output=True,text=True)
 if r.returncode:raise RuntimeError(str(args)+'\n'+r.stdout+r.stderr)
 return r.stdout
modes=[('debug',[],['-O0'],'native-runtime'),('optimized',['-O2'],['-O2'],'task-core-optimized'),('sanitized',['-O2','--sanitize'],['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'],'native-runtime-sanitized'),('thread-sanitized',['-O2','--thread-sanitize'],['-O1','-g','-fsanitize=thread'],'native-runtime-thread-sanitized')]
modes=qualified_variants(modes,a.compiler)
if windows:modes=[mode for mode in modes if mode[0]!='thread-sanitized']
values=[(0,1),(17,3),(2**63-1,0),(-2**63,0),(2**63-1,1),(-2**63,-1),(-2**63,1),(0,0),(17,-1),(-17,3),(2**63-1,2**63-1)]
results=[]
for mode,extra,flags,library in modes:
 for lang,cpp,kind in [('int64','std::int64_t',5),('float32','float',0),('float64','double',1),('Bool','bool',2)]:
  for op,label in [('+','add'),('/','divide'),('%','remainder')]:
   name=f'{mode}-{lang}-{label}';src=a.output/(name+'.k');ir=a.output/(name+'.ll')
   expression=f'(a{op}b)>0' if lang=='Bool' else f'{lang}(a{op}b)'
   src.write_text(f'fn value(a:int64,b:int64)->{lang}:\n  {expression}\nfn main()->{lang}:\n  {"false" if lang=="Bool" else lang+"(0)"}\n')
   run([a.compiler,src,'--test-entry','value','--emit-llvm',ir,'-o',a.output/(name+'-standalone'),*extra]);assert '_mlir_ciface_kalandra_test_entry_ordinary_direct_v1(' in ir.read_text(),name
   code=r'''
#include "FAULT"
#include <bit>
#include <cstdint>
#include <cstdlib>
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
 if(argc!=3)return 2;I a=std::strtoll(argv[1],nullptr,10),b=std::strtoll(argv[2],nullptr,10);
 kalandra::runtime::OrdinaryFault fault{};
#if defined(DIRECT)
 auto value=_mlir_ciface_kalandra_test_entry_ordinary_direct_v1(a,b);
#else
 auto value=_mlir_ciface_kalandra_test_entry_ordinary_v1(a,b,&fault);
#endif
 return kalandra_ordinary_finish_v1(bits(value),KIND,&fault);
}
'''.replace('FAULT',str(root/'native/runtime/OrdinaryFault.h')).replace('TYPE',cpp).replace('KIND',str(kind))
   driver=a.output/(name+'.cpp');driver.write_text(code);runtime=a.compiler.parent/(('' if windows else 'lib')+'kalandra-'+library+('.lib' if windows else '.a'))
   binaries=[]
   for contract in ['recoverable','direct']:
    exe=a.output/(name+'-'+contract+suffix);run([a.clang,'-std=c++23',*flags,'-ffp-contract=off','-Wno-override-module',*(['-fms-runtime-lib=static'] if windows else ['-pthread']),*(['-DDIRECT'] if contract=='direct' else []),driver,ir,runtime,'-o',exe]);binaries.append(exe)
   for x,y in values:
    env=dict(os.environ,KALANDRA_TEST_REPORT_CLEANUP='1')
    records=[subprocess.run([str(exe),str(x),str(y)],env=env,capture_output=True,text=True) for exe in binaries]
    assert [(r.returncode,r.stdout,r.stderr) for r in records][0]==[(r.returncode,r.stdout,r.stderr) for r in records][1],(name,x,y,[(r.returncode,r.stdout,r.stderr) for r in records])
    if records[0].returncode:assert 'cleanup: live=0 allocations=0 releases=0' in records[0].stderr
    results.append(dict(mode=mode,type=lang,operator=label,args=[x,y],status=records[0].returncode,stdout=records[0].stdout,stderr=records[0].stderr))
   print(name,'11 direct/recoverable process outcomes match',flush=True)
(a.output/'results.json').write_text(json.dumps(results,indent=2)+'\n')

# Exercise the compiler-generated main, not only hand-written host adapters.
cli=[]
for mode,extra,flags,library in modes:
 for lang in ['int64','float32','float64','Bool']:
  for label,inputs in [('add',(2**63-1,1)),('divide',(17,0)),('remainder',(17,0))]:
   name=f'{mode}-{lang}-{label}'
   text=(a.output/(name+'.k')).read_text().split('fn main()->')[0]+f'fn main()->{lang}:\n  value({inputs[0]},{inputs[1]})\n'
   source=a.output/(name+'-cli.k');exe=a.output/(name+'-cli'+suffix);source.write_text(text)
   run([a.compiler,source,*extra,'-o',exe])
   env=dict(os.environ,KALANDRA_TEST_REPORT_CLEANUP='1')
   actual=subprocess.run([exe],capture_output=True,text=True,env=env)
   expected=subprocess.run([str(a.output/(name+'-recoverable'+suffix)),*map(str,inputs)],capture_output=True,text=True,env=env)
   assert (actual.returncode,actual.stdout,actual.stderr)==(expected.returncode,expected.stdout,expected.stderr),(name,actual.stderr,expected.stderr)
   cli.append(dict(name=name,status=actual.returncode,stderr=actual.stderr));print(name,'standalone failure matches',flush=True)
(a.output/'cli-results.json').write_text(json.dumps(cli,indent=2)+'\n')
