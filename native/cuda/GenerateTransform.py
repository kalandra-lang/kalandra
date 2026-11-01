"""Port the existing project-owned Metal transform algorithm to CUDA syntax.

Preserves aligned adjacent-pair reduction trees, NaN/Inf behavior and barriers.
CUDA compiler math primitives implement expf/sqrtf; no BLAS/graph library is used.
"""
import argparse
from pathlib import Path
import re
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--kind',choices=('transpose','softmaxRows','rmsNormRows'),required=True)
p.add_argument('--numeric',choices=('int8','uint8','int16','uint16','int32','uint32','int64','uint64','float32','float64'),required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();transpose=a.kind=='transpose';rms=a.kind=='rmsNormRows'
if not transpose and a.numeric!='float32':p.error('row transforms require float32')
header=Path(__file__).resolve().parents[1]/'metal-source/TensorTransformShader.h'
parts=re.findall(r'R"MSL\((.*?)\)MSL"',header.read_text(),re.S);assert len(parts)==3
body=parts[1 if transpose else 2].split(') {',1)[1]
body=body.replace('as_type<float>(p[3])','__uint_as_float(p[3])').replace('as_type<uint>(eps)','__float_as_uint(eps)')
body=body.replace('threadgroup_barrier(mem_flags::mem_device)','__syncthreads()')
body=body.replace('atomic_fetch_min_explicit((device atomic_uint*)(p+5),4u,memory_order_relaxed)','atomicMin(p+5,4u)')
body=body.replace('precise::exp','expf').replace('precise::sqrt','sqrtf').replace('max(a,b)','fmaxf(a,b)')
assert 'threadgroup_barrier' not in body and 'as_type<' not in body and 'device atomic' not in body
width=32 if a.numeric=='float32' else 64 if a.numeric=='float64' else int(a.numeric.removeprefix('u').removeprefix('int'))
carrier='uchar' if transpose else 'float'
text=f'''using uint=unsigned int;
using ulong=unsigned long long;
using uchar=unsigned char;
#define NAN __int_as_float(0x7fc00000)
#define INFINITY __int_as_float(0x7f800000)
constexpr uint kalandra_cell_bytes={width//8};
constexpr bool kalandra_rms={'true' if rms else 'false'};
__device__ __forceinline__ void body(const {carrier}* input,const float* epsilon,
 {carrier}* output,float* scratch,uint* p,uint lane,uint group) {{
'''+body+'\n'
# Full descriptor ABI: allocated pointer, aligned pointer, offset, size, stride.
count=3 if transpose else 5 if rms else 4
args=','.join(f'ulong b{i}{field}' for i in range(count) for field in ('a','p','o','n','s'))
def pointer(index,kind):return f'reinterpret_cast<{kind}*>(b{index}p)+b{index}o'
input_pointer=pointer(0,carrier);output_pointer=pointer(count-2,carrier);parameter_pointer=pointer(count-1,'uint')
epsilon_pointer=pointer(1,'float') if rms else 'nullptr'
scratch_pointer=pointer(2 if rms else 1,'float') if not transpose else 'nullptr'
entry=f'kalandra_tensor_transform_v1_{a.kind}_{a.numeric}_cuda_desc_v1'
text+=f'''extern "C" __global__ __launch_bounds__(256) void {entry}({args}) {{
  body({input_pointer},{epsilon_pointer},{output_pointer},{scratch_pointer},{parameter_pointer},threadIdx.x,blockIdx.x);
}}
'''
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(text)
