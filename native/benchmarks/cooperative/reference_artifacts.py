"""Wrap independently compiled reference shaders in the private dispatch ABI.

This changes entry-point metadata only, never arithmetic or shader instructions.
Validate the resulting SPIR-V with spirv-val and all outputs with Driver.cpp.
"""
import argparse
from pathlib import Path
import struct

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('backend',choices=('cuda','vulkan'))
p.add_argument('input',type=Path)
p.add_argument('output',type=Path)
a=p.parse_args()
entry='kalandra_workgroup_v1_float32_tfloat32r1_g256x'
if a.backend=='cuda':
    ptx=a.input.read_text()
    entry+='_cuda_desc_v1'
    assert entry in ptx and '.extern .func' not in ptx
    header=f'// KALANDRA_CUDA_V1 120 256 0 0 3 {entry} {len(ptx.encode())}'
    tail='// BUFFER 32 -1\n// BUFFER 32 -1\n// BUFFER 32 6\n// PTX\n'+ptx
    checksum=14695981039346656037
    for byte in (header+'\n'+tail).encode():
        checksum=((checksum^byte)*1099511628211)&((1<<64)-1)
    a.output.write_text(header+' '+str(checksum)+'\n'+tail)
else:
    words=list(struct.unpack('<'+'I'*(a.input.stat().st_size//4),a.input.read_bytes()))
    assert words[0]==0x07230203
    result=words[:5];i=5;entries=0
    while i<len(words):
        count,op=words[i]>>16,words[i]&65535
        instruction=words[i:i+count]
        assert count>0 and len(instruction)==count
        if op==15: # OpEntryPoint: model, function ID, string, interface IDs.
            start=3
            end=start
            while not any(b==0 for b in struct.pack('<I',instruction[end])):end+=1
            assert b''.join(struct.pack('<I',w) for w in instruction[start:end+1]).split(b'\0')[0]==b'main'
            name=(entry+'_vk_groups_v1').encode()+b'\0'
            name+=b'\0'*((-len(name))%4)
            instruction=instruction[:start]+list(struct.unpack('<'+'I'*(len(name)//4),name))+instruction[end+1:]
            instruction[0]=(len(instruction)<<16)|op
            entries+=1
        result+=instruction;i+=count
    assert entries==1
    a.output.write_bytes(struct.pack('<'+'I'*len(result),*result))
