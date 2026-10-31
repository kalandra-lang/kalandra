"""Process parity for live private buffers, settled replay and ambiguous values."""
import argparse, json, os, subprocess, sys
from pathlib import Path
from OrdinaryTestEnvironment import configure, qualified_variants
p=argparse.ArgumentParser();p.add_argument('--compiler',type=Path,required=True);p.add_argument('--clang',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
configure(a.clang)
root=Path(__file__).resolve().parents[2];windows=sys.platform=='win32';suffix='.exe' if windows else ''
def run(args,env=None):
 r=subprocess.run(list(map(str,args)),capture_output=True,text=True,env=env)
 if r.returncode:raise RuntimeError(str(args)+'\n'+r.stdout+r.stderr)
 return r.stdout
modes=[('debug',[],['-O0'],'native-runtime'),('optimized',['-O2'],['-O2'],'task-core-optimized'),('sanitized',['-O2','--sanitize'],['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'],'native-runtime-sanitized'),('thread-sanitized',['-O2','--thread-sanitize'],['-O1','-g','-fsanitize=thread'],'native-runtime-thread-sanitized')]
modes=qualified_variants(modes,a.compiler)
if windows:modes=[mode for mode in modes if mode[0]!='thread-sanitized']
fixtures={
 'index':('int64','std::int64_t',5,'  a=[seed,seed+1]\n  a[index]'),
 'map':('int64','std::int64_t',5,'  a=[seed,seed+1]\n  b=map(a,fn(value:int):value/index)\n  b[0]+b[1]'),
 'replay':('int64','std::int64_t',5,'  a=[seed,seed+1]\n  b=[seed+2,seed+3]\n  c=[seed+4,seed+5]\n  d=[seed+6,seed+7]\n  e=[seed+8,seed+9]\n  a[index]+b[0]+c[0]+d[0]+e[0]'),
 'nan':('float64','double',1,'  a=[float64(seed),float64(index)]\n  a[0]/a[1]'),
 'bool':('Bool','bool',2,'  a=[seed,index]\n  a[0] == a[1]'),
 'minimum':('int64','std::int64_t',5,'  a=[seed,index]\n  a[0]'),
}
driver=r'''
#include "FAULT"
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <type_traits>
using I=std::int64_t;using T=TYPE;
extern "C" T _mlir_ciface_kalandra_test_entry_ordinary_direct_v1(I,I);
extern "C" T _mlir_ciface_kalandra_test_entry_ordinary_v1(I,I,kalandra::runtime::OrdinaryFault*);
extern "C" std::int32_t kalandra_ordinary_finish_v1(std::uint64_t,std::int32_t,const kalandra::runtime::OrdinaryFault*) noexcept;
template<class V> std::uint64_t bits(V value){if constexpr(std::is_floating_point_v<V>)return std::bit_cast<std::uint64_t>(value);else return static_cast<std::uint64_t>(value);}
int main(int argc,char **argv){if(argc!=3)return 2;I a=std::strtoll(argv[1],nullptr,10),b=std::strtoll(argv[2],nullptr,10);kalandra::runtime::OrdinaryFault fault{};
#if defined(DIRECT)
 auto value=_mlir_ciface_kalandra_test_entry_ordinary_direct_v1(a,b);
#else
 auto value=_mlir_ciface_kalandra_test_entry_ordinary_v1(a,b,&fault);
#endif
 return kalandra_ordinary_finish_v1(bits(value),KIND,&fault);
}
'''
values=[(0,0),(17,0),(17,1),(17,2),(17,-1),(-2**63,0),(2**63-1,1),(1,1)]
records=[]
for mode,extra,flags,library in modes:
 for fixture,(lang,cpp,kind,body) in fixtures.items():
  name=f'{mode}-{fixture}';src=a.output/(name+'.k');ir=a.output/(name+'.ll');src.write_text(f'fn value(seed:int64,index:int64)->{lang}:\n{body}\nfn main()->{lang}:\n  value(17,1)\n')
  run([a.compiler,src,'--test-entry','value','--emit-llvm',ir,'-o',a.output/(name+'-standalone'+suffix),*extra])
  text=ir.read_text();assert '_mlir_ciface_kalandra_test_entry_ordinary_direct_v1(' in text,name
  # These fixtures must actually exercise private buffers/replay, not only a
  # scalar expression that the frontend optimized out.
  assert 'kalandra.ordinary.local-buffers' in text,name
  code=a.output/(name+'.cpp');code.write_text(driver.replace('FAULT',str(root/'native/runtime/OrdinaryFault.h')).replace('TYPE',cpp).replace('KIND',str(kind)))
  runtime=a.compiler.parent/(('' if windows else 'lib')+'kalandra-'+library+('.lib' if windows else '.a'));bins=[]
  for contract in ['recoverable','direct']:
   exe=a.output/(name+'-'+contract+suffix);run([a.clang,'-std=c++23',*flags,'-ffp-contract=off','-Wno-override-module',*(['-fms-runtime-lib=static'] if windows else ['-pthread']),*(['-DDIRECT'] if contract=='direct' else []),code,ir,runtime,'-o',exe]);bins.append(exe)
  for fail in [0,1,2,3,4,5,7,20]:
   for x,y in values:
    env=dict(os.environ,KALANDRA_TEST_REPORT_CLEANUP='1',KALANDRA_TEST_FAIL_ALLOC_AT=str(fail))
    outcomes=[subprocess.run([str(exe),str(x),str(y)],env=env,capture_output=True,text=True) for exe in bins]
    data=[(r.returncode,r.stdout,r.stderr) for r in outcomes];assert data[0]==data[1],(name,fail,x,y,data)
    if data[0][0]:assert data[0][0]>0 and 'cleanup: live=0 allocations=0 releases=0' in data[0][2],(name,data)
    records.append(dict(mode=mode,fixture=fixture,allocation_failure=fail,args=[x,y],status=data[0][0],stdout=data[0][1],stderr=data[0][2]))
  print(name,'64 direct/recoverable buffer process outcomes match',flush=True)
(a.output/'results.json').write_text(json.dumps(records,indent=2)+'\n')
