"""Isolated direct A*B^T screen, preserving adjacent-pair float32 arithmetic."""
import argparse,json,os,sys,re
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);a=parser.parse_args();assert not a.output.exists()
build=p.ROOT/'benchmarks/.build/gpu-transpose-matmul-2026-10-29/screen';build.mkdir(parents=True,exist_ok=True)
root=p.ROOT/'native/metal-source'
source='#include <metal_stdlib>\nusing namespace metal;\n'
for name in ('HardwareShaders.h','SkinnyMatmul.h'):
 text=(root/name).read_text()
 for body in re.findall(r'R"MSL\((.*?)\)MSL"',text,re.S):source+=body
source+='''
kernel void transpose_input(device const uint* b [[buffer(0)]],device uint* bt [[buffer(1)]],device const uint* p [[buffer(2)]],uint i [[thread_position_in_grid]]) {
 uint k=p[1],n=p[2];if(i<k*n)bt[i]=b[(i%n)*k+i/n];
}
kernel void direct_nt(device const float* a [[buffer(0)]],device const float* b [[buffer(1)]],device float* out [[buffer(2)]],device const uint* p [[buffer(3)]],uint tid [[thread_position_in_grid]],uint lane [[thread_index_in_simdgroup]]) {
 uint m=p[0],k=p[1],n=p[2],cell=tid/32,row=cell/max(n,1u),col=cell%max(n,1u);
 if(cell>=m*n)return;
 float stack[27];float tail=0.0f;uint full=k/32;
 for(uint block=0;block<full+uint(k%32!=0);++block){
  uint at=block*32+lane,live=min(32u,k-block*32);
  float v=at<k?a[row*k+at]*b[col*k+at]:0.0f;
  for(uint step=1;step<32;step*=2){float other=simd_shuffle_down(v,step);if(lane%(step*2)==0&&lane+step<live)v=v+other;}
  if(lane==0){if(block<full){uint occupied=block,level=0;while(occupied&1u){v=stack[level]+v;++level;occupied>>=1;}stack[level]=v;}else tail=v;}
 }
 if(lane==0){float v=tail;bool used=k%32!=0;for(uint level=0,occupied=full;occupied;++level,occupied>>=1)if(occupied&1u){v=used?stack[level]+v:stack[level];used=true;}out[cell]=v;}
}
'''
msl=build/'study.metal';msl.write_text(source)
lock=json.loads((p.ROOT/'native/metal-proof/toolchain.json').read_text());metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
lib=build/'study.metallib';p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',msl,'-o',build/'study.air']);p.run([*metal,build/'study.air','-o',lib])
driver=Path(__file__).with_name('TransposeMatmulStudy.mm');binary=build/'study'
p.run([p.CLANG,'-std=c++23','-O2','-ffp-contract=off','-fobjc-arc','-fblocks',driver,'-framework','Foundation','-framework','Metal','-o',binary])
shapes=[(1,64,1),(1,512,1),(16,512,16),(64,512,64),(3,37,5),(17,33,9),(64,2048,64)]
checks=[]
for shape in shapes:
 for v in ('baseline','current'):
  rows=p.observations(p.run([binary,lib,v,*shape,1,1],env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')),shape[0]*shape[2],1,1)
  checks.append(dict(shape=shape,variant=v,passed=True))
cells=[(v,*s) for s in shapes for v in ('baseline','current')]
def invoke(c,count,rounds):return p.observations(p.run([binary,lib,*c,count,rounds]),c[1]*c[3],count,rounds)
r=p.measure_paired(cells,invoke,lambda c:c[1]*c[3],blocks=3,target_ms=10,adaptive=True,max_iterations=256)
r.update(contract=__doc__,checks=checks,scope='Preallocated inputs and outputs; one command batches repeated kernels. Baseline includes a transpose dispatch. This excludes language/runtime overhead and does not establish whole-program speedup.')
p.evidence(a.output,r,[Path(__file__),driver,msl,root/'HardwareShaders.h',root/'SkinnyMatmul.h',p.ROOT/'native/metal/MatmulLayout.h'],[binary,lib],p.ROOT/'build/native/native/kalandra')
