"""Strict IEEE recurrence bits and checked countdown failure across four builds."""
import argparse,json,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--compiler',type=Path,required=True);p.add_argument('--clang',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2];windows=sys.platform=='win32'
def run(args):
 r=subprocess.run(list(map(str,args)),capture_output=True,text=True)
 if r.returncode:raise RuntimeError(str(args)+'\n'+r.stdout+r.stderr)
 return r.stdout
variants=[('debug',[],['-O0'],'native-runtime'),('optimized',['-O2'],['-O2'],'task-core-optimized'),('sanitized',['-O2','--sanitize'],['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'],'native-runtime-sanitized'),('thread-sanitized',['-O2','--thread-sanitize'],['-O1','-g','-fsanitize=thread'],'native-runtime-thread-sanitized')]
results=[]
for mode,extra,flags,library in variants:
 if windows and mode=='thread-sanitized':continue
 for language,cpp in [('float32','float'),('float64','double')]:
  for fixture in ('finite','external','zero'):
   name=mode+'-'+language+'-'+fixture;source=a.output/(name+'.k');ir=a.output/(name+'.ll');driver=a.output/(name+'.cpp');exe=a.output/(name+('.exe' if windows else ''))
   scale='0' if fixture=='zero' else '1.015625';initial=f'{language}(x)*{language}(0.25)' if fixture=='finite' else 'x';parameter='int64' if fixture=='finite' else language
   source.write_text(f'fn iterate(x:{language},rounds:int64)->{language}:\n  if rounds == 0:\n    x\n  else:\n    iterate(x*{language}({scale})+{language}(0.25),rounds-1)\nfn value(x:{parameter},rounds:int64)->{language}:\n  iterate({initial},rounds)\nfn main()->{language}:\n  {language}(0)\n')
   run([a.compiler,source,'--test-entry','value','--emit-llvm',ir,'-o',a.output/(name+'-standalone'),*extra]);assert '_mlir_ciface_kalandra_test_entry_ordinary_v1(' in ir.read_text()
   if mode=='optimized':assert 'kalandra.ordinary.countdown' in ir.read_text()
   text=r'''
#include "RUNTIME"
#include "FAULT"
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
using T=TYPE;using I=std::int64_t;using Input=INPUT;
extern "C" T _mlir_ciface_kalandra_test_entry(Input,I,kalandra::runtime::Invocation*);
extern "C" T _mlir_ciface_kalandra_test_entry_ordinary_v1(Input,I,kalandra::runtime::OrdinaryFault*);
unsigned cases=0;
void test(Input input,I rounds){
 using namespace kalandra::runtime;
 Invocation context;auto expected=_mlir_ciface_kalandra_test_entry(input,rounds,&context);
 OrdinaryFault fault;std::memset(&fault,0xa5,sizeof(fault));auto before=fault;
 auto value=_mlir_ciface_kalandra_test_entry_ordinary_v1(input,rounds,&fault);
 bool failed=ordinaryFaulted(value,fault);
 if(failed!=(rounds<0) || failed!=(context.status()!=0))std::abort();
 if(failed) {
  if(fault.code!=context.status() || fault.line!=context.line() || fault.column!=context.column())std::abort();
 } else {
  if(std::memcmp(&value,&expected,sizeof(T)))std::abort();
  T reference=T(input);if(FINITE)reference=reference*T(.25);
  for(I i=0;i<rounds;++i){volatile T product=reference*T(SCALE);reference=product+T(.25);}
  if(std::isnan(reference)){if(!std::isnan(value) || fault.code)std::abort();}
  else if(std::memcmp(&value,&reference,sizeof(T)))std::abort();
 }
 if(!ordinaryAmbiguous(value) && std::memcmp(&before,&fault,sizeof(fault)))std::abort();
 context.settle();if(context.live() || context.allocations()!=context.releases())std::abort();++cases;
}
int main(){
 constexpr I lo=std::numeric_limits<I>::min();
 for(Input value:std::array<Input,5>{Input(-31),Input(-1),Input(0),Input(1),Input(31)})
  for(I rounds:std::array<I,10>{0,1,2,16,64,257,50000,lo,lo+1,lo+2})test(value,rounds);
 if constexpr(!FINITE) {
  for(T value:std::array<T,8>{T(-0.0),std::numeric_limits<T>::denorm_min(),std::numeric_limits<T>::max(),-std::numeric_limits<T>::max(),std::numeric_limits<T>::infinity(),-std::numeric_limits<T>::infinity(),std::numeric_limits<T>::quiet_NaN(),std::numeric_limits<T>::signaling_NaN()})
   for(I rounds:std::array<I,6>{0,1,2,16,lo,lo+2})test(value,rounds);
 }
 std::printf("%u strict recurrence/countdown/fault cases passed\n",cases);
}
'''
   text=text.replace('RUNTIME',str(root/'native/runtime/Runtime.h')).replace('FAULT',str(root/'native/runtime/OrdinaryFault.h')).replace('TYPE',cpp).replace('INPUT','std::int64_t' if fixture=='finite' else cpp).replace('FINITE','true' if fixture=='finite' else 'false').replace('SCALE',scale)
   driver.write_text(text);runtime=a.compiler.parent/(('' if windows else 'lib')+'kalandra-'+library+('.lib' if windows else '.a'));host=['-fms-runtime-lib=static'] if windows else ['-pthread']
   run([a.clang,'-std=c++23',*flags,'-ffp-contract=off','-Wno-override-module',*host,driver,ir,runtime,'-o',exe]);output=run([exe]);print(name,output.strip(),flush=True);results.append(dict(mode=mode,type=language,fixture=fixture,result=output.strip()))
(a.output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
