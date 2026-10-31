"""Compare the ordinary scalar ABI with checked execution and exact/bit oracles."""
import argparse, json, os, subprocess, sys
from OrdinaryPairChecks import pair_checks
from pathlib import Path
from OrdinaryTestEnvironment import configure, qualified_variants
p=argparse.ArgumentParser();p.add_argument('--compiler',type=Path,required=True);p.add_argument('--clang',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--buffer-entry',action='store_true');p.add_argument('--pair-entry',action='store_true');p.add_argument('--direct-entry',action='store_true');a=p.parse_args();a.pair_entry|=a.direct_entry
configure(a.clang)
root=Path(__file__).resolve().parents[2];a.output.mkdir(parents=True,exist_ok=True);windows=sys.platform=='win32'
def run(cmd, expected=0):
 r=subprocess.run(list(map(str,cmd)),text=True,capture_output=True)
 if r.returncode!=expected:raise RuntimeError(str(cmd)+'\n'+r.stdout+r.stderr)
 return r.stdout
entries=[(f'{s}int{w}',f'std::{s}int{w}_t',False) for s in ('','u') for w in (8,16,32,64)]+[('float32','float',True),('float64','double',True),('Bool','bool',False)]
variants=[('debug',[],['-O0'],'native-runtime'),('optimized',['-O2'],['-O2'],'task-core-optimized'),('sanitized',['-O2','--sanitize'],['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'],'native-runtime-sanitized'),('thread-sanitized',['-O2','--thread-sanitize'],['-O1','-g','-fsanitize=thread'],'native-runtime-thread-sanitized')]
variants=qualified_variants(variants,a.compiler)
records=[]
for mode,extra,flags,library in variants:
 if windows and mode=='thread-sanitized':continue
 for language,cpp,floating in entries:
  boolean=language=='Bool';name=mode+'-'+language;source=a.output/(name+'.k');ir=a.output/(name+'.ll');driver=a.output/(name+'.cpp');exe=a.output/(name+('.exe' if windows else ''))
  # Numeric additions exercise both success and checked failures. Identity
  # preserves floating payloads; Bool identity exercises false/true ABI extension.
  operation='a' if floating or boolean else 'a+b';main='false' if boolean else language+'(0)'
  body=f'  {operation}'
  if a.buffer_entry:
   body='  values=[a,b]\n  '+('values[0]' if floating or boolean else 'values[0]+values[1]')
   # The pool handles numeric backing; exercise Bool's scalar ABI over it.
   if boolean:body='  values=[1,2]\n  if a:\n    values[0] == 1\n  else:\n    values[1] == 0'
  source.write_text(f'fn value(a:{language},b:{language})->{language}:\n{body}\nfn main()->{language}:\n  {main}\n')
  run([a.compiler,source,'--test-entry','value','--emit-llvm',ir,'-o',a.output/(name+'-standalone'),*extra])
  assert '_mlir_ciface_kalandra_test_entry_ordinary_v1(' in ir.read_text(),name
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
#include <type_traits>
using T=TYPE;
extern "C" T _mlir_ciface_kalandra_test_entry(T,T,kalandra::runtime::Invocation*);
extern "C" T _mlir_ciface_kalandra_test_entry_ordinary_v1(T,T,kalandra::runtime::OrdinaryFault*);
template<class V> bool same(V a,V b) { return std::memcmp(&a,&b,sizeof(V))==0; }
std::uint64_t cases=0;
void test(T a,T b) {
  using namespace kalandra::runtime;
  Invocation context;
  T reference=_mlir_ciface_kalandra_test_entry(a,b,&context);
  OrdinaryFault info; std::memset(&info,0xa5,sizeof(info));
  auto original=info;
  T value=_mlir_ciface_kalandra_test_entry_ordinary_v1(a,b,&info);
  bool failed=ordinaryFaulted(value,info);
  if(failed!=(context.status()!=0))std::abort();
  if(failed) {
    if(info.code!=context.status() || info.line!=context.line() || info.column!=context.column())std::abort();
  } else if(!same(reference,value))std::abort();
  if(!ordinaryAmbiguous(value) && std::memcmp(&original,&info,sizeof(info)))std::abort();
  if(ordinaryAmbiguous(value) && !failed && info.code)std::abort();
  context.settle();
  if(context.live() || context.allocations()!=context.releases())std::abort();
  ++cases;
}
template<class V> void verify() {
 if constexpr(std::is_floating_point_v<V>) {
   using Bits=std::conditional_t<sizeof(V)==4,std::uint32_t,std::uint64_t>;
   std::array<std::uint64_t,12> values={0,0x8000000000000000ULL,1,0x0010000000000000ULL,0x3ff0000000000000ULL,0x7fefffffffffffffULL,0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8000000000123ULL,0x7ff0000000000123ULL,0xfff8000000000123ULL,0xfff0000000000123ULL};
   for(auto bits:values) {
     Bits raw;
     if constexpr(sizeof(V)==4) {
       // Explicit float32 payloads rather than rounded float64 conversions.
       static constexpr std::array<std::uint32_t,12> narrow={0,0x80000000U,1,0x00800000U,0x3f800000U,0x7f7fffffU,0x7f800000U,0xff800000U,0x7fc00123U,0x7f800123U,0xffc00123U,0xff800123U};
       for(auto x:narrow) { V v=std::bit_cast<V>(x);test(v,V(1)); }
       break;
     } else { raw=Bits(bits);test(std::bit_cast<V>(raw),V(1)); }
   }
 } else if constexpr(std::is_same_v<V,bool>) {test(false,false);test(true,true);}
 else {
   std::array<V,7> values={V(0),V(1),V(2),std::numeric_limits<V>::max(),std::numeric_limits<V>::min(),V(-1),V(std::numeric_limits<V>::max()/2)};
   for(V x:values)for(V y:values) {
     __int128 sum=static_cast<__int128>(x)+static_cast<__int128>(y);
     bool overflow=sum<static_cast<__int128>(std::numeric_limits<V>::min()) || sum>static_cast<__int128>(std::numeric_limits<V>::max());
     kalandra::runtime::OrdinaryFault info;
     V result=_mlir_ciface_kalandra_test_entry_ordinary_v1(x,y,&info);
     if(kalandra::runtime::ordinaryFaulted(result,info)!=overflow)std::abort();
     if(!overflow && result!=V(sum))std::abort();
     test(x,y);
   }
 }
}
int main(){verify<T>();std::printf("%llu ordinary exact/first-fault/payload cases passed\n",(unsigned long long)cases);}
'''
  text=text.replace('RUNTIME',str(root/'native/runtime/Runtime.h')).replace('FAULT',str(root/'native/runtime/OrdinaryFault.h')).replace('TYPE',cpp)
  if a.pair_entry:
   if not a.buffer_entry:assert '_mlir_ciface_kalandra_test_entry_ordinary_pair_v1(' in ir.read_text(),name
   if a.direct_entry:assert '_mlir_ciface_kalandra_test_entry_ordinary_direct_v1(' in ir.read_text(),name
   if 'T reference=' in text:
    text=text.replace('template<class V> bool same',pair_checks('T','T')+'\ntemplate<class V> bool same')
    text=text.replace('T reference=_mlir_ciface_kalandra_test_entry(a,b,&context);','T reference=_mlir_ciface_kalandra_test_entry(a,b,&context);checkPair(a,b,reference,context);')
   else:
    text=text.replace('int main(){',pair_checks('I','I')+'\nint main(){')
    text=text.replace('auto expected=_mlir_ciface_kalandra_test_entry(a,b,&context);','auto expected=_mlir_ciface_kalandra_test_entry(a,b,&context);checkPair(a,b,expected,context);')
  driver.write_text(text)
  runtime=a.compiler.parent/(('' if windows else 'lib')+'kalandra-'+library+('.lib' if windows else '.a'))
  host=['-fms-runtime-lib=static'] if windows else ['-pthread']
  run([a.clang,'-std=c++23',*(['-DTEST_DIRECT'] if a.direct_entry else []),*(['-DSKIP_PAIR'] if windows or a.buffer_entry else []),*flags,'-ffp-contract=off','-Wno-override-module',*host,driver,ir,runtime,'-o',exe])
  output=run([exe]);print(mode,language,output.strip(),flush=True);records.append(dict(mode=mode,type=language,result=output.strip()))
# Fault-producing projections cover both NaN widths and Bool's true sentinel.
for mode,extra,flags,library in variants:
 if windows and mode=='thread-sanitized':continue
 for language,cpp,operation,main in [('Bool','bool','a+b>0','false'),('float32','float','float32(a+b)','float32(0)'),('float64','double','float64(a+b)','float64(0)')]:
  name=mode+'-fault-'+language;source=a.output/(name+'.k');ir=a.output/(name+'.ll');driver=a.output/(name+'.cpp');exe=a.output/(name+('.exe' if windows else ''))
  body='  '+operation
  if a.buffer_entry:body='  values=[a,b]\n  '+operation.replace('a+b','values[0]+values[1]')
  source.write_text(f'fn value(a:int64,b:int64)->{language}:\n{body}\nfn main()->{language}:\n  {main}\n')
  run([a.compiler,source,'--test-entry','value','--emit-llvm',ir,'-o',a.output/(name+'-standalone'),*extra])
  assert '_mlir_ciface_kalandra_test_entry_ordinary_v1(' in ir.read_text(),name
  text=r'''
#include "RUNTIME"
#include "FAULT"
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
using I=std::int64_t;using T=TYPE;
extern "C" T _mlir_ciface_kalandra_test_entry(I,I,kalandra::runtime::Invocation*);
extern "C" T _mlir_ciface_kalandra_test_entry_ordinary_v1(I,I,kalandra::runtime::OrdinaryFault*);
int main(){
 using namespace kalandra::runtime;
 constexpr I lo=std::numeric_limits<I>::min(),hi=std::numeric_limits<I>::max();
 for(auto [a,b]:std::array<std::array<I,2>,6>{{{0,0},{hi,0},{hi,1},{lo,-1},{lo,0},{0,1}}}) {
  Invocation context;auto expected=_mlir_ciface_kalandra_test_entry(a,b,&context);
  OrdinaryFault fault;std::memset(&fault,0xa5,sizeof(fault));auto before=fault;
  auto value=_mlir_ciface_kalandra_test_entry_ordinary_v1(a,b,&fault);
  bool overflow=(a==hi && b==1)||(a==lo && b==-1);
  if(ordinaryFaulted(value,fault)!=overflow || (context.status()!=0)!=overflow)std::abort();
  if(overflow) {
   if(fault.code!=context.status() || fault.line!=context.line() || fault.column!=context.column())std::abort();
  } else if(std::memcmp(&value,&expected,sizeof(T)))std::abort();
  if(!ordinaryAmbiguous(value) && std::memcmp(&before,&fault,sizeof(fault)))std::abort();
 }
 std::puts("6 ordinary projected arithmetic fault/ABI cases passed");
}
'''
  if a.pair_entry:
   if not a.buffer_entry:assert '_mlir_ciface_kalandra_test_entry_ordinary_pair_v1(' in ir.read_text(),name
   if a.direct_entry:assert '_mlir_ciface_kalandra_test_entry_ordinary_direct_v1(' in ir.read_text(),name
   text=text.replace('int main(){',pair_checks('I','I')+'\nint main(){')
   text=text.replace('auto expected=_mlir_ciface_kalandra_test_entry(a,b,&context);','auto expected=_mlir_ciface_kalandra_test_entry(a,b,&context);checkPair(a,b,expected,context);')
  driver.write_text(text.replace('RUNTIME',str(root/'native/runtime/Runtime.h')).replace('FAULT',str(root/'native/runtime/OrdinaryFault.h')).replace('TYPE',cpp))
  runtime=a.compiler.parent/(('' if windows else 'lib')+'kalandra-'+library+('.lib' if windows else '.a'))
  host=['-fms-runtime-lib=static'] if windows else ['-pthread']
  run([a.clang,'-std=c++23',*(['-DTEST_DIRECT'] if a.direct_entry else []),*(['-DSKIP_PAIR'] if windows or a.buffer_entry else []),*flags,'-ffp-contract=off','-Wno-override-module',*host,driver,ir,runtime,'-o',exe])
  output=run([exe]);print(mode,language,output.strip(),flush=True);records.append(dict(mode=mode,type=language,projection=True,result=output.strip()))
(a.output/'results.json').write_text(json.dumps(records,indent=2)+'\n')
