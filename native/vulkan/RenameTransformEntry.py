"""Give predefined GLSL SPIR-V a typed, versioned compiler/runtime entry ABI."""
import argparse
from pathlib import Path
import struct
p=argparse.ArgumentParser()
p.add_argument('source',type=Path)
p.add_argument('output',type=Path)
p.add_argument('kind',choices=('transpose','softmaxRows','rmsNormRows'))
p.add_argument('type')
a=p.parse_args()
raw=a.source.read_bytes()
assert len(raw)%4==0
words=list(struct.unpack('<'+'I'*(len(raw)//4),raw))
assert len(words)>=5 and words[0]==0x07230203
out=words[:5]
at=5;found=False
entry=None;cap_at=None;extension_at=None;mode_at=None
floating=[];decorated=set();type_at=None
while at<len(words):
    count,opcode=words[at]>>16,words[at]&0xffff
    assert count and at+count<=len(words)
    instruction=words[at:at+count]
    if opcode==17:
        cap_at=len(out)+count
    if opcode in (11,14) and extension_at is None:
        extension_at=len(out)
    if opcode==16:
        mode_at=len(out)+count
    if 19<=opcode<=39 and type_at is None:
        type_at=len(out)
    if opcode in (129,131,133,136): # FAdd/FSub/FMul/FDiv
        assert count>=5
        floating.append(instruction[2])
    if opcode==71 and count==3 and instruction[2]==42:
        decorated.add(instruction[1]) # NoContraction already present
    if opcode==15: # OpEntryPoint: keep execution model, function and interface IDs.
        assert not found and count>=4 and words[at+1]==5
        old=struct.pack('<'+'I'*(count-3),*instruction[3:])
        end=old.index(b'\0');name_words=(end+1+3)//4
        assert old[:end]==b'main'
        name=f'kalandra_tensor_transform_v1_{a.kind}_{a.type}_vk_groups_v1'.encode()+b'\0'
        name+=b'\0'*((-len(name))%4)
        encoded=list(struct.unpack('<'+'I'*(len(name)//4),name))
        instruction=[0,instruction[1],instruction[2],*encoded,*instruction[3+name_words:]]
        instruction[0]=(len(instruction)<<16)|15
        entry=instruction[2]
        found=True
    out+=instruction;at+=count
assert found and at==len(words)
assert type_at is not None
# Match the existing separately rounded row implementation. This is not an
# optimized matmul bundle: leave contraction disabled on every scalar FP op.
annotations=[]
for identifier in floating:
    if identifier not in decorated:
        annotations += [(3<<16)|71,identifier,42]
out[type_at:type_at]=annotations
if a.kind!='transpose':
    # Request actual device guarantees instead of relying on default NSZ/NaN
    # assumptions. Runtime admission checks these float32 properties first.
    assert entry is not None and cap_at is not None and extension_at is not None and mode_at is not None
    extension=b'SPV_KHR_float_controls\0'
    extension+=b'\0'*((-len(extension))%4)
    encoded=list(struct.unpack('<'+'I'*(len(extension)//4),extension))
    additions=[(mode_at,[(4<<16)|16,entry,4461,32]),
               (extension_at,[((len(encoded)+1)<<16)|10,*encoded]),
               (cap_at,[(2<<16)|17,4466])]
    for offset,extra in sorted(additions,key=lambda item:item[0],reverse=True):
        out[offset:offset]=extra

a.output.write_bytes(struct.pack('<'+'I'*len(out),*out))
