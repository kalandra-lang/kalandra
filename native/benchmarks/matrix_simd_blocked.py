"""Benchmark-only blocked SIMD-group matrix primitive; different rounding is explicit."""
from pathlib import Path
base=Path(__file__).with_name('matrix_simd_architecture.py');body=base.read_text()
start=body.index(" source=(base/'study.metal').read_text()+r'''");end=body.index(" (b/'study.metal').write_text(source)",start)
shader=r'''
#include <metal_simdgroup_matrix>
kernel void kalandra_simdgroup_matrix(
 device KalandraHardwareMatrix& a [[buffer(0)]],
 device KalandraHardwareMatrix& b [[buffer(1)]],
 device KalandraHardwareMatrix& output [[buffer(2)]],
 device KalandraHardwareShape& parameters [[buffer(3)]],
 uint lane [[thread_index_in_threadgroup]],uint2 group [[threadgroup_position_in_grid]]) {
 threadgroup float aa[1024],bb[1024];
 const uint k=parameters.values[1],n=parameters.values[2],rb=group.y*32,cb=group.x*32;
 const uint sg=lane/32,sr=(sg/2)*16,sc=(sg%2)*16;
 auto c00=make_filled_simdgroup_matrix<float,8,8>(0.0f);
 auto c01=make_filled_simdgroup_matrix<float,8,8>(0.0f);
 auto c10=make_filled_simdgroup_matrix<float,8,8>(0.0f);
 auto c11=make_filled_simdgroup_matrix<float,8,8>(0.0f);
 for(uint first=0;first<k;first+=32) {
  for(uint at=lane;at<1024;at+=128) {
   aa[at]=a.values[(rb+at/32)*k+first+at%32];
   bb[at]=b.values[(first+at/32)*n+cb+at%32];
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
  for(uint off=0;off<32;off+=8) {
   simdgroup_float8x8 a0,a1,b0,b1;
   simdgroup_load(a0,aa+sr*32+off,32);
   simdgroup_load(a1,aa+(sr+8)*32+off,32);
   simdgroup_load(b0,bb+off*32+sc,32);
   simdgroup_load(b1,bb+off*32+sc+8,32);
   simdgroup_multiply_accumulate(c00,a0,b0,c00);
   simdgroup_multiply_accumulate(c01,a0,b1,c01);
   simdgroup_multiply_accumulate(c10,a1,b0,c10);
   simdgroup_multiply_accumulate(c11,a1,b1,c11);
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
 }
 simdgroup_store(c00,output.values+(rb+sr)*n+cb+sc,n);
 simdgroup_store(c01,output.values+(rb+sr)*n+cb+sc+8,n);
 simdgroup_store(c10,output.values+(rb+sr+8)*n+cb+sc,n);
 simdgroup_store(c11,output.values+(rb+sr+8)*n+cb+sc+8,n);
}
'''
body=body[:start]+" source=(base/'study.metal').read_text()+"+repr(shader)+'\n'+body[end:]
body=body.replace('sources=[Path(__file__),driver',"sources=[Path(__file__),Path(__file__).with_name('matrix_simd_blocked.py'),driver")
body=body.replace("'candidate?8:16'","'candidate?32:16'").replace('columns==16?256:32','columns==16?256:128')
body=body.replace('m%16==0 && n%16==0','m%32==0 && n%32==0')
exec(compile(body,str(base),'exec'),dict(__file__=str(base),__name__='__main__'))
