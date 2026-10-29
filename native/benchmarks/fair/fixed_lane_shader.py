"""Constant-source SIMD sharing control; no dynamically indexed lane shuffle."""
from lane_share_shader import lane_kernel as dynamic_kernel

def lane_kernel(source,mode):
    assert mode=='fixed'
    text=dynamic_kernel(source,'both')
    for i in range(8):
        old=f'x{i}=simd_shuffle(x{i},ushort((row&1u)*16u));'
        assert text.count(old)==1
        text=text.replace(old,f'float lo{i}=simd_broadcast(x{i},ushort(0));\n  float hi{i}=simd_broadcast(x{i},ushort(16));\n  x{i}=(row&1u) ? hi{i} : lo{i};')
        old=f'y{i}=simd_shuffle(y{i},ushort(col));'
        assert text.count(old)==1
        text=text.replace(old,f'float flip{i}=simd_shuffle_xor(y{i},ushort(16));\n  y{i}=(row&1u) ? flip{i} : y{i};')
    return text.replace('_lane_both(', '_lane_fixed(').replace('kalandra_lane_eight16_both','kalandra_lane_eight16_fixed')
