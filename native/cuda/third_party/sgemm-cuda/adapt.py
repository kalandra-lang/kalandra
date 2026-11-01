"""Reproducible private full-float32 kernel adapter; verifies pristine MIT input.

The tile-specialized resource is not enabled in the language runtime yet.
"""
from pathlib import Path
import hashlib
import json
import re

def source():
    root=Path(__file__).parent
    manifest=json.loads((root/'upstream.json').read_text())
    for name,digest in manifest['files'].items():
        assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest, name
    text=(root/'10_kernel_warptiling.cuh').read_text()
    # Only unused host/library headers are removed; no cuBLAS code is imported.
    text='using uint = unsigned int;\n'+re.sub(r'^#include[^\n]*\n','',text,flags=re.M).replace('#pragma once','')
    old='__global__ void __launch_bounds__(NUM_THREADS)'
    assert text.count(old)==1
    text=text.replace(old,'__device__ __forceinline__ void')
    # Kalandra's product is AB, not alpha*AB+beta*C. In particular beta=0 must
    # never read uninitialized output (nor inherit a NaN from unrelated backing).
    pattern=r'float4 tmp = reinterpret_cast<float4 \*>\(\s*&C_interim\[\(threadRowInWarp \* TM \+ resIdxM\) \* N \+\s*threadColInWarp \* TN \+ resIdxN\]\)\[0\];'
    text,count=re.subn(pattern,'float4 tmp;',text)
    assert count==1
    for component,index in zip('xyzw',range(4)):
        old=f'tmp.{component} = alpha * threadResults[i + {index}] + beta * tmp.{component};'
        assert text.count(old)==1
        text=text.replace(old,f'tmp.{component} = threadResults[i + {index}];')
    for tile,warp,iterations,tm,tn in [(64,32,2,4,4),(128,64,4,8,4)]:
        text+=f'''
extern "C" __global__ __launch_bounds__(128)
void kalandra_sgemm_warp{tile}(int M,int N,int K,float *A,float *B,float *C) {{
  sgemmWarptiling<{tile},{tile},16,{warp},{warp},{iterations},{tm},{tn},128>(M,N,K,1.0f,A,B,0.0f,C);
}}
'''
    args=','.join(f'unsigned long long b{i}{field}' for i in range(4) for field in ('a','p','o','n','s'))
    for tile,warp,iterations,tm,tn in [(64,32,2,4,4),(128,64,4,8,4)]:
        text+=f'''
extern "C" __global__ __launch_bounds__(128)
void kalandra_numeric_v1_matmul_float32_tile{tile}_cuda_desc_v1({args}) {{
  const unsigned *p=reinterpret_cast<const unsigned*>(b3p)+b3o;
  sgemmWarptiling<{tile},{tile},16,{warp},{warp},{iterations},{tm},{tn},128>(p[0],p[2],p[1],1.0f,
    reinterpret_cast<float*>(b0p)+b0o,reinterpret_cast<float*>(b1p)+b1o,0.0f,reinterpret_cast<float*>(b2p)+b2o);
}}
'''
    return text
if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path)
    a=p.parse_args();text=source()
    if a.output:a.output.write_text(text)
    else:print('PASS: pinned MIT source/license and deterministic AB-only warp-tile adapter')
