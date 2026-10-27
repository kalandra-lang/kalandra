"""Deferred reduction validation against checked execution and a wide integer oracle."""
import argparse,json,os,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--compiler',type=Path,required=True);p.add_argument('--clang',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True);root=Path(__file__).resolve().parents[2];windows=sys.platform=='win32'
def run(args,env=None):
 r=subprocess.run(list(map(str,args)),env=env,capture_output=True,text=True)
 if r.returncode:raise RuntimeError(str(args)+'\n'+r.stdout+r.stderr)
 return r.stdout
variants=[('debug',[],['-O0'],'native-runtime'),('optimized',['-O2'],['-O2'],'task-core-optimized'),('sanitized',['-O2','--sanitize'],['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'],'native-runtime-sanitized'),('thread-sanitized',['-O2','--thread-sanitize'],['-O1','-g','-fsanitize=thread'],'native-runtime-thread-sanitized')]
results=[]
for mode,extra,flags,library in variants:
 if windows and mode=='thread-sanitized':continue
 for width in (2,8,32,128):
  for lang,cpp in [('int64','std::int64_t'),('float64','double'),('Bool','bool')]:
   name=f'{mode}-{lang}-{width}';source=a.output/(name+'.k');ir=a.output/(name+'.ll');driver=a.output/(name+'.cpp');exe=a.output/(name+('.exe' if windows else ''))
   values=','.join('a' if i%2==0 else 'b' for i in range(width));value='acc' if lang=='int64' else 'acc != 0' if lang=='Bool' else 'float64(acc)';main='false' if lang=='Bool' else lang+'(0)'
   source.write_text(f'fn total(values:[int64],index:int64,acc:int64)->{lang}:\n  if index == {width}:\n    {value}\n  else:\n    total(values,index+1,acc+values[index])\nfn value(a:int64,b:int64)->{lang}:\n  total([{values}],0,0)\nfn main()->{lang}:\n  {main}\n')
   run([a.compiler,source,'--test-entry','value','--emit-llvm',ir,'-o',a.output/(name+'-standalone'),*extra])
   if width>=32:assert 'kalandra.ordinary.reduction' in ir.read_text(),name
   text=r'''
#include "RUNTIME"
#include "FAULT"
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
using I=std::int64_t;using Result=TYPE;
extern "C" Result _mlir_ciface_kalandra_test_entry(I,I,kalandra::runtime::Invocation*);
extern "C" Result _mlir_ciface_kalandra_test_entry_ordinary_v1(I,I,kalandra::runtime::OrdinaryFault*);
int main(){
 using namespace kalandra::runtime;constexpr I lo=std::numeric_limits<I>::min(),hi=std::numeric_limits<I>::max();
 std::array<I,15> inputs={lo,lo+1,-4294967296LL,-1,0,1,65535,65536,2147483647LL,2147483648LL,4294967295LL,4294967296LL,1099511627776LL,hi/128,hi};
 unsigned cases=0;
 for(auto a:inputs)for(auto b:inputs) {
  Invocation context;auto expected=_mlir_ciface_kalandra_test_entry(a,b,&context);
  OrdinaryFault info;std::memset(&info,0xa5,sizeof(info));auto before=info;
  auto value=_mlir_ciface_kalandra_test_entry_ordinary_v1(a,b,&info);bool failed=ordinaryFaulted(value,info);
  if(failed!=(context.status()!=0))std::abort();
  if(failed){if(info.code!=context.status() || info.line!=context.line() || info.column!=context.column())std::abort();}
  else if(std::memcmp(&value,&expected,sizeof(Result)))std::abort();
  __int128 sum=0;bool overflow=false;
  for(unsigned i=0;i<WIDTH;++i){sum+=i%2?b:a;if(sum<lo || sum>hi){overflow=true;break;}}
  // Explicit injected allocation failures precede arithmetic. Otherwise this
  // independent wide accumulator must agree, including intermediate overflow.
  if(!std::getenv("KALANDRA_TEST_FAIL_ALLOC_AT") || std::strtoull(std::getenv("KALANDRA_TEST_FAIL_ALLOC_AT"),nullptr,10)==0) {
   if(failed!=overflow)std::abort();
   if(!overflow && value!=Result(I(sum)))std::abort();
  }
  if(!ordinaryAmbiguous(value) && std::memcmp(&before,&info,sizeof(info)))std::abort();
  context.settle();if(context.live() || context.allocations()!=context.releases())std::abort();++cases;
 }
 std::printf("%u deferred reduction/range replay/first-fault cases passed\n",cases);
}
'''
   text=text.replace('RUNTIME',str(root/'native/runtime/Runtime.h')).replace('FAULT',str(root/'native/runtime/OrdinaryFault.h')).replace('TYPE',cpp).replace('WIDTH',str(width));driver.write_text(text)
   runtime=a.compiler.parent/(('' if windows else 'lib')+'kalandra-'+library+('.lib' if windows else '.a'));host=['-fms-runtime-lib=static'] if windows else ['-pthread']
   run([a.clang,'-std=c++23',*flags,'-ffp-contract=off','-Wno-override-module',*host,driver,ir,runtime,'-o',exe])
   for failure in (0,1,2,3,5,17):
    output=run([exe],dict(os.environ,KALANDRA_TEST_FAIL_ALLOC_AT=str(failure)));print(name,failure,output.strip(),flush=True);results.append(dict(mode=mode,type=lang,width=width,failure=failure,result=output.strip()))
(a.output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
