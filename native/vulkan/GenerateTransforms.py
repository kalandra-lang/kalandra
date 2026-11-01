"""Build predefined GLSL transforms from pinned/owned implementations.

Transpose adapts ggml's MIT copy_transpose.comp tile function. Row math reuses
Kalandra's existing Metal body to preserve its adjacent-pair reduction contract,
NaN/infinity handling and invalid-epsilon precedence. This is a closed, checked
translation of predefined source, not an arbitrary Metal shader translator.
"""
import argparse
from pathlib import Path
import re

p=argparse.ArgumentParser()
p.add_argument('--upstream',type=Path,required=True)
p.add_argument('--metal',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
types={'int8':1,'uint8':1,'int16':2,'uint16':2,'int32':4,'uint32':4,
       'int64':8,'uint64':8,'float32':4,'float64':8}

def replace(source,old,new,*,count=None):
    actual=source.count(old)
    assert actual and (count is None or actual==count),(old,actual,count)
    return source.replace(old,new)

def write(kind,name,text):
    (a.output/f'{kind}-{name}.comp').write_text(text)

upstream=(a.upstream/'copy_transpose.comp').read_text()
start=upstream.index('void iter(uvec3 wg_id)')
end=upstream.index('#define CEIL_DIV',start)
tile=upstream[start:end]
tile=replace(tile,'gl_LocalInvocationID.x','(gl_LocalInvocationID.x % 32u)',count=1)
tile=replace(tile,'gl_LocalInvocationID.y','(gl_LocalInvocationID.x / 32u)',count=1)
# p[1]/p[2] are the original row-major input rows/columns. Two uint32
# planes preserve each 64-bit cell without any floating arithmetic/conversion.
fields={'ne00':'p[1]','ne01':'p[2]','ne02':'PLANES','ne03':'1u',
        'ne10':'p[1]','ne11':'p[2]','ne12':'PLANES','ne13':'1u',
        'nb00':'(p[2]*PLANES)','nb01':'PLANES','nb02':'1u','nb03':'0u',
        'nb10':'PLANES','nb11':'(p[1]*PLANES)','nb12':'1u','nb13':'0u'}
for field,value in fields.items():
    tile=replace(tile,'p.'+field,value)
assert 'p.' not in tile and 'gl_LocalInvocationID.y' not in tile
for name,width in types.items():
    extension=''
    if width==1:
        extension='#extension GL_EXT_shader_8bit_storage : require\n#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require\n'
    elif width==2:
        extension='#extension GL_EXT_shader_16bit_storage : require\n#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require\n'
    cell='uint8_t' if width==1 else 'uint16_t' if width==2 else 'uint'
    header=f'''#version 450
// Tile adapted from ggml copy_transpose.comp, MIT, pinned in UPSTREAM.json.
// Copyright (c) 2023-2026 The ggml authors. See third_party/ggml/LICENSE.
#extension GL_EXT_control_flow_attributes : require
{extension}#define A_TYPE {cell}
#define D_TYPE {cell}
#define PLANES {2 if width==8 else 1}u
#define TILE_DIM 32
layout(local_size_x=256,local_size_y=1,local_size_z=1) in;
layout(set=0,binding=0,std430) readonly buffer Input {{ A_TYPE data_a[]; }};
layout(set=0,binding=1,std430) writeonly buffer Output {{ D_TYPE data_d[]; }};
layout(set=0,binding=2,std430) buffer Parameters {{ uint p[6]; }};
shared uint sh[TILE_DIM][TILE_DIM+1];
uint get_aoffset() {{ return 0; }}
uint get_doffset() {{ return 0; }}
'''
    main='''void main() {
  uint group=gl_WorkGroupID.x+gl_WorkGroupID.y*gl_NumWorkGroups.x;
  uint columns=p[1]/32u+uint(p[1]%32u!=0u);
  uint rows=p[2]/32u+uint(p[2]%32u!=0u);
  if(group>=columns*rows) return;
  for(uint plane=0;plane<PLANES;++plane) {
    if(plane!=0u) barrier();
    iter(uvec3(group%columns,group/columns,plane));
  }
}
'''
    write('transpose',name,header+tile+main)

blocks=re.findall(r'R"MSL\((.*?)\)MSL"',a.metal.read_text(),re.S)
assert len(blocks)==3, 'Predefined Metal transform source layout changed'
row=blocks[-1]
body_start=row.index(']]) {')+len(']]) {')
body_end=row.rindex('\n}')
body=row[body_start:body_end]
body=replace(body,'as_type<float>(p[3])','uintBitsToFloat(p[3])',count=1)
body=replace(body,'as_type<uint>(eps)','floatBitsToUint(eps)',count=1)
body=replace(body,'atomic_fetch_min_explicit((device atomic_uint*)(p+5),4u,memory_order_relaxed)',
             'atomicMin(p[5],4u)',count=1)
body=replace(body,'threadgroup_barrier(mem_flags::mem_device);','memoryBarrierBuffer(); barrier();')
body=replace(body,'precise::exp','exp',count=1)
body=replace(body,'precise::sqrt','sqrt',count=1)
body=re.sub(r'\bulong\b','uint64_t',body)
body=re.sub(r'\bNAN\b','uintBitsToFloat(0x7fc00000u)',body)
body=re.sub(r'\bINFINITY\b','uintBitsToFloat(0x7f800000u)',body)
body=re.sub(r'\binput\b','data_input',body)
body=re.sub(r'\boutput\b','data_output',body)
assert not any(token in body for token in ('device ', 'as_type', 'threadgroup_', 'precise::'))
for kind in ('softmaxRows','rmsNormRows'):
    rms=kind=='rmsNormRows'
    current=body
    if not rms:
        current=replace(current,'const float eps=kalandra_rms?(p[4]?epsilon[0]:uintBitsToFloat(p[3])):1.0f;',
                         'const float eps=1.0f;',count=1)
        assert 'epsilon[' not in current
    else:
        current=replace(current,'p[4]?','(p[4]!=0u)?',count=1)
    current=replace(current,'!epsilonBits','epsilonBits==0u',count=1)
    current=replace(current,'(epsilonBits&0x80000000u)',
                    '((epsilonBits&0x80000000u)!=0u)',count=1)
    current=replace(current,'if(!lane)','if(lane==0u)',count=1)
    current=replace(current,'!p[0]','p[0]==0u',count=1)
    # GLSL indexes storage arrays with uint32. Host admission bounds every
    # logical count/byte range; retain wide loop/address intermediates until
    # after the same guards as the original implementation.
    current=re.sub(r'\b(data_input|data_output|scratch|epsilon)\[([^\[\]]+)\]',
                   r'\1[uint(\2)]',current)
    epsilon='layout(set=0,binding=1,std430) readonly buffer Epsilon { float epsilon[]; };\n' if rms else ''
    scratch_binding=2 if rms else 1
    header=f'''#version 450
// Predefined Kalandra Metal row implementation, preserving the existing contract.
#extension GL_ARB_gpu_shader_int64 : require
layout(local_size_x=256,local_size_y=1,local_size_z=1) in;
layout(set=0,binding=0,std430) readonly buffer Input {{ float data_input[]; }};
{epsilon}layout(set=0,binding={scratch_binding},std430) buffer Scratch {{ float scratch[]; }};
layout(set=0,binding={scratch_binding+1},std430) buffer Output {{ float data_output[]; }};
layout(set=0,binding={scratch_binding+2},std430) buffer Parameters {{ uint p[6]; }};
const bool kalandra_rms={'true' if rms else 'false'};
void main() {{
  uint lane=gl_LocalInvocationID.x;
  uint group=gl_WorkGroupID.x+gl_WorkGroupID.y*gl_NumWorkGroups.x;
'''
    write(kind,'float32',header+current+'\n}\n')
