"""Strict 32-lane operand-sharing controls, with explicit virtual lane mapping.

All 32 lanes participate in each shuffle. A values come from lanes 0/16; B values
come from lanes 0..15. Only complete output tiles may use these entries. The host
must check threadExecutionWidth == 32. Reduction order and barriers stay intact.
"""
from output_reuse_shader import function

def lane_kernel(source, mode):
    assert mode in ('mapping','a','b','both')
    entry='kalandra_matmul_hardware_register16_grid2d_aligned'
    body=function(source,'kernel void '+entry+'(')
    body=body.replace(entry+'(',entry+'_lane_'+mode+'(')
    old='uint lane [[thread_index_in_threadgroup]],';assert body.count(old)==1
    body=body.replace(old,'uint simdLane [[thread_index_in_simdgroup]],\n    uint simdGroup [[simdgroup_index_in_threadgroup]],')
    old='  threadgroup float av[1024],bv[1024];';assert body.count(old)==1
    body=body.replace(old,'  const uint lane=simdGroup*32+simdLane;\n'+old)
    if mode=='mapping':return body
    name='kalandra_lane_eight16_'+mode
    helper='inline float '+name+'(threadgroup float* a, threadgroup float* b, uint row, uint col, uint first) {\n'
    for i in range(8):
        x=f'a[row*64+first+{i}]';y=f'b[(first+{i})*16+col]'
        if mode in ('a','both'):
            helper+=f'  float x{i}=col==0 ? {x} : 0.0f;\n  x{i}=simd_shuffle(x{i},ushort((row&1u)*16u));\n';x=f'x{i}'
        if mode in ('b','both'):
            helper+=f'  float y{i}=(row&1u)==0 ? {y} : 0.0f;\n  y{i}=simd_shuffle(y{i},ushort(col));\n';y=f'y{i}'
        helper+=f'  float p{i}={x}*{y};\n'
    helper+='  return ((p0+p1)+(p2+p3))+((p4+p5)+(p6+p7));\n}\n'
    assert body.count('kalandra_hardware_eight16(')==8
    return helper+body.replace('kalandra_hardware_eight16(',name+'(')
