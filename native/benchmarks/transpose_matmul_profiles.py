"""Follow-up transposed matrix profiles: shared-tile locality and bounded roots.

Requires the frozen first screen; no production code is changed. Each matrix
cell uses the same adjacent-pair arithmetic. Inputs are non-dyadic and mixed
scale to make incorrect reassociation observable.
"""
import argparse,json,os,sys,re
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);a=parser.parse_args();assert not a.output.exists()
build=p.ROOT/'benchmarks/.build/gpu-transpose-matmul-2026-10-29/profiles';build.mkdir(parents=True,exist_ok=True)
frozen=p.ROOT/'benchmarks/.build/gpu-transpose-matmul-2026-10-29/screen/study.metal'
source=frozen.read_text().replace('float stack[27];float tail=0.0f;uint full=k/32;','float stack[8];float tail=0.0f;uint full=k/32;')
start=source.index('struct KalandraHardwareMatrix');end=source.index('kernel void transpose_input')
nt=source[start:end].replace('KalandraHardware','NtKalandraHardware').replace('kalandra_','nt_kalandra_')
# Coalesced global reads, writing the transposed tile into threadgroup storage.
old='bv[lane] = col < n && inner + lane / 8 < k ? b.values[(inner + lane / 8) * n + col] : 0.0f;'
new='uint loadCol=(group%tileColumns)*8+lane/8; bv[(lane%8)*8+lane/8] = loadCol<n && inner+lane%8<k ? b.values[loadCol*k+inner+lane%8] : 0.0f;'
assert nt.count(old)==1;nt=nt.replace(old,new)
for cols,leaf in ((8,32),(8,64),(16,64)):
 old=f'bv[at]=c<n ? b.values[(first+at/{cols})*n+c] : 0.0f;'
 new=f'bv[(at%{leaf})*{cols}+at/{leaf}]=c<n ? b.values[c*k+first+at%{leaf}] : 0.0f;'
 name='nt_kalandra_matmul_hardware_'+('register16' if cols==16 else 'block'+str(leaf))
 at=nt.index('kernel void '+name);stop=nt.find('kernel void ',at+10);stop=len(nt) if stop<0 else stop
 part=nt[at:stop];assert part.count(old)==1,(cols,leaf)
 assert part.count(f'c=colBase+at%{cols}')==1
 part=part.replace(f'c=colBase+at%{cols}',f'c=colBase+at/{leaf}').replace(old,new)
 nt=nt[:at]+part+nt[stop:]
nt,count=re.subn(r'b.values\[\(first\+(\d+)\)\*n\+col\]',r'b.values[col*k+first+\1]',nt);assert count==32
source+=nt
msl=build/'study.metal';msl.write_text(source)
driver_source=Path(__file__).with_name('TransposeMatmulStudy.mm');text=driver_source.read_text()
text=text.replace('if(variant=="baseline") {','if(variant=="baseline" || variant=="nt_skinny" || variant=="nt_tile") {',1)
text=text.replace('bool skinny=useSkinnyMatmul(m,k,n)', 'bool skinny=variant!="nt_tile" && useSkinnyMatmul(m,k,n)',1)
text=text.replace('threads=skinny||reg?256:wide?128:64;', 'if(variant!="baseline")entry="nt_"+entry;\n  threads=skinny||reg?256:wide?128:64;',1)
text=text.replace('check(m&&k&&n&&count&&count<=256);','check(m&&k&&k<=4096&&n&&count&&count<=256);')
text=text.replace('av[i]=float(int(i%17)-8)/8;', 'av[i]=float((int(i%17)-8)*0.134567+(i%29==0?10000.0003:0.00043));')
text=text.replace('bv[i]=float(int(i%13)-6)/8;', 'bv[i]=float((int(i%13)-6)*0.173219+(i%31==0?-0.000023:0.00019));')
driver=build/'Study.mm';text=text.replace('#include "../metal/MatmulLayout.h"','#include "'+str(p.ROOT/'native/metal/MatmulLayout.h')+'"');driver.write_text(text)
lock=json.loads((p.ROOT/'native/metal-proof/toolchain.json').read_text());metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal'];lib=build/'study.metallib'
p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',msl,'-o',build/'study.air']);p.run([*metal,build/'study.air','-o',lib])
binary=build/'study';p.run([p.CLANG,'-std=c++23','-O2','-ffp-contract=off','-fobjc-arc','-fblocks',driver,'-framework','Foundation','-framework','Metal','-o',binary])
shapes=[(1,64,1),(1,512,1),(16,512,16),(64,512,64),(3,37,5),(17,33,9),(64,2048,64)]
checks=[]
for shape in shapes:
 for v in ('baseline','current','nt_skinny','nt_tile'):
  p.observations(p.run([binary,lib,v,*shape,1,1],env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')),shape[0]*shape[2],1,1)
  checks.append(dict(shape=shape,variant=v,passed=True))
results={}
for version in ('current','nt_skinny','nt_tile'):
 print(version,flush=True)
 cells=[(v,*s) for s in shapes for v in ('baseline','current')]
 def invoke(c,count,rounds):return p.observations(p.run([binary,lib,'baseline' if c[0]=='baseline' else version,*c[1:],count,rounds]),c[1]*c[3],count,rounds)
 results[version]=p.measure_paired(cells,invoke,lambda c:c[1]*c[3],blocks=3,target_ms=10,adaptive=True,max_iterations=256)
p.evidence(a.output,dict(contract=__doc__,checks=checks,variants=results,scope='Isolated kernels, preallocated storage, one command with repeated iterations; not whole-program timing.'),[Path(__file__),driver_source,driver,msl,frozen,p.ROOT/'native/metal/MatmulLayout.h'],[binary,lib],p.ROOT/'build/native/native/kalandra')
