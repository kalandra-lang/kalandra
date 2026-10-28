"""Bounded Euclidean recurrences: exact values, fallback and first faults."""
import argparse,json,subprocess,sys
from pathlib import Path
from OrdinaryPairChecks import pair_checks
from OrdinaryTestEnvironment import configure
p=argparse.ArgumentParser();p.add_argument('--compiler',type=Path,required=True);p.add_argument('--clang',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
windows=sys.platform=='win32'
configure(a.clang)
def run(args):
 r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=180)
 if r.returncode:raise RuntimeError(str(args)+'\n'+r.stdout+r.stderr)
 return r.stdout
variants=[('debug',[],['-O0'],'native-runtime'),('optimized',['-O2'],['-O2'],'task-core-optimized'),('sanitized',['-O2','--sanitize'],['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'],'native-runtime-sanitized'),('thread-sanitized',['-O2','--thread-sanitize'],['-O1','-g','-fsanitize=thread'],'native-runtime-thread-sanitized')]
results=[]
for mode,extra,flags,library in variants:
 if windows and mode=='thread-sanitized':continue
 for language,cpp in [('float32','float'),('float64','double')]:
  for fixture,ac,ao,bc,bo in [('positive',13,7,7,11),('other-positive',5,1,3,2),('cycle',1,0,0,3),('u32-divisor',13,7,65537,0),('wide-divisor',13,7,65538,17),('zero-divisor',13,7,0,0),('negative-divisor',13,7,-7,-11),('overflowing-divisor',13,7,9223372036854775807,11)]:
   name=mode+'-'+language+'-'+fixture;source=a.output/(name+'.k');ir=a.output/(name+'.ll');driver=a.output/(name+'.cpp');exe=a.output/(name+('.exe' if windows else ''))
   source.write_text(f'fn gcd(a:int64,b:int64)->int64:\n  if b == 0:\n    a\n  else:\n    gcd(b,a % b)\nfn iterate(x:int64,seed:int64,rounds:int64)->{language}:\n  if rounds == 0:\n    {language}(x)\n  else:\n    iterate(gcd(x*{ac}+{ao},seed*{bc}+{bo})+seed,seed,rounds-1)\nfn value(seed:int64,rounds:int64)->{language}:\n  iterate(seed,seed,rounds)\nfn main()->{language}:\n  {language}(0)\n')
   run([a.compiler,source,'--test-entry','value','--emit-llvm',ir,'-o',a.output/(name+'-standalone'),*extra])
   if mode=='optimized' and fixture in ('positive','other-positive'):assert 'kalandra.ordinary.integer-cases' in ir.read_text(),name
   if fixture=='positive':
    for marker in ('kalandra.ordinary.stable-countdown','kalandra.ordinary.integer-single-case'):assert marker in ir.read_text(),(name,marker)
   if mode=='optimized' and fixture=='negative-divisor':assert 'kalandra.ordinary.integer-cases' not in ir.read_text(),name
   text=r'''
#include "RUNTIME"
#include "FAULT"
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
using T=TYPE;using I=std::int64_t;using Input=I;using Wide=__int128;
extern "C" T _mlir_ciface_kalandra_test_entry(I,I,kalandra::runtime::Invocation*);
extern "C" T _mlir_ciface_kalandra_test_entry_ordinary_v1(I,I,kalandra::runtime::OrdinaryFault*);
PAIR_CHECKS
unsigned cases=0;
bool fits(Wide x){return x>=std::numeric_limits<I>::min()&&x<=std::numeric_limits<I>::max();}
void test(I seed,I rounds){
 using namespace kalandra::runtime;Invocation context;auto expected=_mlir_ciface_kalandra_test_entry(seed,rounds,&context);checkPair(seed,rounds,expected,context);
 OrdinaryFault fault;std::memset(&fault,0xa5,sizeof(fault));auto before=fault;auto value=_mlir_ciface_kalandra_test_entry_ordinary_v1(seed,rounds,&fault);
 bool failed=ordinaryFaulted(value,fault);if(failed!=(context.status()!=0))std::abort();
 if(failed){if(fault.code!=context.status()||fault.line!=context.line()||fault.column!=context.column())std::abort();}
 else if(std::memcmp(&value,&expected,sizeof(T)))std::abort();
 if(rounds>=0){
  I x=seed;bool valid=true;
  for(I n=0;n<rounds&&valid;++n){
   Wide wa=Wide(x)*AC+AO,wb=Wide(seed)*BC+BO;if(!fits(wa)||!fits(wb)){valid=false;break;}
   I aa=I(wa),bb=I(wb);while(bb){if(aa==std::numeric_limits<I>::min()&&bb==-1){valid=false;break;}I rr=aa%bb;aa=bb;bb=rr;}
   Wide sum=Wide(aa)+seed;if(!fits(sum))valid=false;else x=I(sum);
  }
  if(valid==failed)std::abort();if(valid){T reference=T(x);if(std::memcmp(&value,&reference,sizeof(T)))std::abort();}
 }
 if(!ordinaryAmbiguous(value)&&std::memcmp(&before,&fault,sizeof(fault)))std::abort();
 context.settle();if(context.live()||context.allocations()!=context.releases())std::abort();++cases;
}
int main(int argc,char**){
 constexpr I lo=std::numeric_limits<I>::min(),hi=std::numeric_limits<I>::max();
 if(argc>1){
  // Independent integer reference reaches an actual fixed point before
  // fast-forwarding; no checked implementation is assumed for huge counts.
  for(I seed:std::array<I,3>{0,1,17}){
   I x=seed;bool stable=false;
   for(unsigned k=0;k<1000&&!stable;++k){
    Wide wa=Wide(x)*AC+AO,wb=Wide(seed)*BC+BO;if(!fits(wa)||!fits(wb))std::abort();
    I aa=I(wa),bb=I(wb);while(bb){I rr=aa%bb;aa=bb;bb=rr;}
    Wide sum=Wide(aa)+seed;if(!fits(sum))std::abort();I next=I(sum);stable=next==x;x=next;
   }
   if(!stable)std::abort();T expected=T(x);
   for(I n:std::array<I,2>{1000000000000LL,hi}){
    auto actual=_mlir_ciface_kalandra_test_entry_ordinary_direct_v1(seed,n);
    if(std::memcmp(&actual,&expected,sizeof(T)))std::abort();
   }
  }
  std::puts("6 huge-countdown fixed-point cases passed");return 0;
 }
 for(I seed:std::array<I,12>{lo,lo+1,hi,hi-1,-65536,-31,-1,0,1,65535,65536,65537})
  for(I n:std::array<I,8>{0,1,2,16,257,lo,lo+1,lo+2})test(seed,n);
 for(I seed=0;seed<65536;++seed)for(I n:std::array<I,3>{0,1,2})test(seed,n);
 for(I seed:std::array<I,5>{0,1,17,65534,65535})
  for(I n:std::array<I,7>{15,16,255,256,65535,65536,65537})test(seed,n);
 std::printf("%u integer-domain/IEEE/first-fault cases passed\n",cases);
}
'''
   text=text.replace('PAIR_CHECKS',pair_checks('Input','I')).replace('RUNTIME',str(root/'native/runtime/Runtime.h')).replace('FAULT',str(root/'native/runtime/OrdinaryFault.h')).replace('TYPE',cpp)
   for key,value in [('AC',ac),('AO',ao),('BC',bc),('BO',bo)]:text=text.replace(key,f'Wide({value}LL)')
   driver.write_text(text);runtime=a.compiler.parent/(('' if windows else 'lib')+'kalandra-'+library+('.lib' if windows else '.a'));host=['-fms-runtime-lib=static'] if windows else ['-pthread']
   run([a.clang,'-std=c++23','-DTEST_DIRECT',*flags,'-ffp-contract=off','-Wno-override-module',*host,driver,ir,runtime,'-o',exe]);output=run([exe]);print(name,output.strip(),flush=True);results.append(dict(mode=mode,type=language,fixture=fixture,result=output.strip()))
   if fixture=='positive':
    huge=subprocess.run([str(exe),'huge'],capture_output=True,text=True,timeout=10)
    if huge.returncode:raise RuntimeError(name+' huge countdown failed: '+huge.stdout+huge.stderr)
    results.append(dict(mode=mode,type=language,fixture=fixture,huge_countdown=True,result=huge.stdout.strip()))
(a.output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
