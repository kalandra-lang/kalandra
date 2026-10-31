"""Qualify private ordinary buffers, replay and unchanged allocation failures."""
import argparse,json,os,subprocess,sys
from pathlib import Path
from OrdinaryTestEnvironment import configure, qualified_variants
p=argparse.ArgumentParser();p.add_argument('--compiler',type=Path,required=True);p.add_argument('--clang',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--direct-entry',action='store_true');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True);root=Path(__file__).resolve().parents[2];windows=sys.platform=='win32'
configure(a.clang)
def run(cmd,env=None):
 r=subprocess.run(list(map(str,cmd)),env=env,text=True,capture_output=True)
 if r.returncode:raise RuntimeError(str(cmd)+'\n'+r.stdout+r.stderr)
 return r.stdout
source=(root/'native/benchmarks/fair/RuntimeWorkloads.k').read_text()
fixtures={'closure':(source,'closure',3),'rounds':(source,'arrays',0),'alias':('''fn value(seed:int,rounds:int)->float64:
  a=[seed,seed+1,seed+2,seed+3]
  alias=a
  b=map(a,fn(value:int):value*2)
  float64(alias[0]+alias[1]+alias[2]+alias[3]+a[0]+a[1]+a[2]+a[3]+b[0]+b[1]+b[2]+b[3]+rounds)
fn main()->float64:
  value(17,16)
''','value',1),'replay':('''fn value(seed:int,rounds:int)->float64:
  a=[seed,seed+1]
  b=[seed+2,seed+3]
  c=[seed+4,seed+5]
  d=[seed+6,seed+7]
  e=[seed+8,seed+9]
  float64(a[0]+a[1]+b[0]+b[1]+c[0]+c[1]+d[0]+d[1]+e[0]+e[1]+rounds)
fn main()->float64:
  value(17,16)
''','value',2)}
for width in (16,32,64,128):
 values=','.join('seed' if i==0 else f'seed+{i}' for i in range(width))
 text=f'fn total(values:[int],index:int,acc:int)->float64:\n  if index == {width}:\n    float64(acc)\n  else:\n    total(values,index+1,acc+values[index])\nfn iterate(values:[int],rounds:int)->float64:\n  if rounds == 0:\n    total(values,0,0)\n  else:\n    iterate(map(values,fn(value:int):(value*13+7)%1000003),rounds-1)\nfn value(seed:int,rounds:int)->float64:\n  iterate([{values}],rounds)\nfn main()->float64:\n  value(17,16)\n'
 fixtures['wide'+str(width)]=(text,'value',width+10)
driver=r'''
#include "RUNTIME"
#include "FAULT"
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
using I=std::int64_t;
extern "C" double _mlir_ciface_kalandra_test_entry(I,I,kalandra::runtime::Invocation*);
extern "C" double _mlir_ciface_kalandra_test_entry_ordinary_v1(I,I,kalandra::runtime::OrdinaryFault*);
#if defined(TEST_DIRECT)
extern "C" double _mlir_ciface_kalandra_test_entry_ordinary_direct_v1(I,I);
#endif
std::uint64_t cases=0;
void test(I seed,I rounds,bool independent) {
 using namespace kalandra::runtime;
 Invocation context;
 double reference=_mlir_ciface_kalandra_test_entry(seed,rounds,&context);
 OrdinaryFault info;std::memset(&info,0xa5,sizeof(info));auto before=info;
 double value=_mlir_ciface_kalandra_test_entry_ordinary_v1(seed,rounds,&info);
 bool failed=ordinaryFaulted(value,info);
 if(failed!=(context.status()!=0))std::abort();
 if(failed) {
  if(info.code!=context.status() || info.line!=context.line() || info.column!=context.column())std::abort();
 } else if(std::bit_cast<std::uint64_t>(value)!=std::bit_cast<std::uint64_t>(reference))std::abort();
 if(!ordinaryAmbiguous(value) && std::memcmp(&before,&info,sizeof(info)))std::abort();
#if defined(TEST_DIRECT)
 if(!failed){auto direct=_mlir_ciface_kalandra_test_entry_ordinary_direct_v1(seed,rounds);if(std::bit_cast<std::uint64_t>(direct)!=std::bit_cast<std::uint64_t>(reference))std::abort();}
#endif
 if(independent && !failed) {
  I expected=0;
  if(FIXTURE>=10) {
   std::array<I,128> values{};for(I i=0;i<FIXTURE-10;++i)values[i]=seed+i;
   for(I n=0;n<rounds;++n)for(I i=0;i<FIXTURE-10;++i)values[i]=(values[i]*13+7)%1000003;
   for(I i=0;i<FIXTURE-10;++i)expected+=values[i];
  } else if(FIXTURE==0) {
   std::array<I,8> values{};for(I i=0;i<8;++i)values[i]=seed+i;
   for(I n=0;n<rounds;++n)for(auto &x:values)x=(x*13+7)%1000003;
   for(auto x:values)expected+=x;
  } else if(FIXTURE==1)expected=16*seed+24+rounds;
  else if(FIXTURE==2)expected=10*seed+45+rounds;
  else {expected=seed;for(I i=0;i<rounds;++i)expected=(expected*13+seed+7)%1000003;}
  if(value!=double(expected))std::abort();
 }
 context.settle();if(context.live() || context.allocations()!=context.releases())std::abort();++cases;
}
int main(){
 for(I seed:std::array<I,9>{-31,-1,0,1,17,31,65535,65536,1000004})
  for(I rounds:std::array<I,6>{0,1,2,16,64,257})test(seed,rounds,true);
 for(I seed:std::array<I,4>{std::numeric_limits<I>::min(),std::numeric_limits<I>::max(),std::numeric_limits<I>::max()/13,std::numeric_limits<I>::min()/13})
  for(I rounds:std::array<I,3>{0,1,16})test(seed,rounds,false);
 std::printf("%llu ordinary private-buffer/alias/fault/replay cases passed\n",(unsigned long long)cases);
}
'''
driver=driver.replace('RUNTIME',str(root/'native/runtime/Runtime.h')).replace('FAULT',str(root/'native/runtime/OrdinaryFault.h'))
variants=[('debug',[],['-O0'],'native-runtime'),('optimized',['-O2'],['-O2'],'task-core-optimized'),('sanitized',['-O2','--sanitize'],['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'],'native-runtime-sanitized'),('thread-sanitized',['-O2','--thread-sanitize'],['-O1','-g','-fsanitize=thread'],'native-runtime-thread-sanitized')]
variants=qualified_variants(variants,a.compiler)
results=[]
for mode,extra,flags,library in variants:
 if windows and mode=='thread-sanitized':continue
 for name,(text,entry,kind) in fixtures.items():
  stem=mode+'-'+name;src=a.output/(stem+'.k');ir=a.output/(stem+'.ll');cpp=a.output/(stem+'.cpp');exe=a.output/(stem+('.exe' if windows else ''));src.write_text(text)
  run([a.compiler,src,'--test-entry',entry,'--emit-llvm',ir,'-o',a.output/(stem+'-standalone'),*extra]);assert '_mlir_ciface_kalandra_test_entry_ordinary_v1(' in ir.read_text(),stem
  if a.direct_entry:assert '_mlir_ciface_kalandra_test_entry_ordinary_direct_v1(' in ir.read_text(),stem
  cpp.write_text(driver.replace('FIXTURE',str(kind)))
  runtime=a.compiler.parent/(('' if windows else 'lib')+'kalandra-'+library+('.lib' if windows else '.a'))
  run([a.clang,'-std=c++23',*(['-DTEST_DIRECT'] if a.direct_entry else []),*flags,'-ffp-contract=off','-Wno-override-module',*(['-fms-runtime-lib=static'] if windows else ['-pthread']),cpp,ir,runtime,'-o',exe])
  for fail in (0,1,2,3,4,5,7,20):
   env=dict(os.environ,KALANDRA_TEST_FAIL_ALLOC_AT=str(fail));out=run([exe],env);print(mode,name,fail,out.strip(),flush=True);results.append(dict(mode=mode,fixture=name,allocation_failure=fail,result=out.strip()))
(a.output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
