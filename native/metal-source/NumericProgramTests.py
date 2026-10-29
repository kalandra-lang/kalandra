"""Resident typed graph composition and selected-path numerical/shape obligations."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
parser=argparse.ArgumentParser()
parser.add_argument('--compiler',required=True)
parser.add_argument('--root',type=Path,required=True)
parser.add_argument('--variant',choices=('debug','optimized','sanitized','thread-sanitized'))
args=parser.parse_args();args.root.mkdir(parents=True,exist_ok=True)
flags=[] if not args.variant else ['--target','metal',{'debug':'-O0','optimized':'-O2','sanitized':'--sanitize','thread-sanitized':'--thread-sanitize'}[args.variant]]
names=('int8','uint8','int16','uint16','int32','uint32','int64','uint64','float32','float64')
widths=(8,8,16,16,32,32,64,64,32,64)
programs=0

def check(name,case,body,expected,*,rank=1,scale=0,empty=False,tensor=False,extra=False,helper=False,fault=0,target=None,one=False):
    global programs
    target=target or name;width=widths[names.index(target)]
    shape=','.join(['?']*rank);dimensions=','.join(['1']*(rank-1)+['0' if empty else '1' if one else '3'])
    literal='[]' if empty else '[1]' if one else '[1,2,3]'
    for _ in range(rank-1):literal='['+literal+']'
    result=f'Tensor[{target},{shape}]' if tensor else target
    prefix=f'fn exact(values:Tensor[{name},2])->{name}:\n  reduceSum(values)\n' if helper else ''
    param=f',other:Tensor[{name},{shape}]' if extra else ''
    other_dimensions=','.join(['1']*(rank-1)+['2'])
    other_literal='[4,5]'
    for _ in range(rank-1):other_literal='['+other_literal+']'
    setup=(f'    other:Tensor[{name},{other_dimensions}]=tensor({other_literal})\n    second=try await transfer(other,gpu)\n') if extra else ''
    source=(prefix+f'fn operation(values:Tensor[{name},{shape}],scale:{name}{param})->{result}:\n'+
            ''.join('  '+line+'\n' for line in body.splitlines())+
            f'fn main()->{result} ! TaskError:\n  try parallel:\n    gpu=devices.gpu()\n'
            f'    values:Tensor[{name},{dimensions}]=tensor({literal})\n    resident=try await transfer(values,gpu)\n'+setup+
            f'    answer=try await spawn operation(resident,{scale}{",second" if extra else ""}) on gpu\n'+
            ('    try await transfer(answer,devices.cpu())\n' if tensor else '    answer\n'))
    path,destination=args.root/f'{name}-{case}.k',args.root/f'{name}-{case}'
    path.write_text(source)
    built=subprocess.run(list(map(str,[args.compiler,"--arithmetic","software",path,*flags,'-o',destination])),capture_output=True,text=True,timeout=180)
    assert built.returncode==0,(path,built.returncode,built.stdout,built.stderr)
    if args.variant:
        def bits(v):return int.from_bytes(struct.pack('<f' if width==32 else '<d',v),'little') if target.startswith('float') else v & ((1<<width)-1)
        output=','.join(f'{bits(v):0{width//4}x}' for v in expected)
        if tensor:output='['+output+']'
        for workers in (1,2,4):
            ran=subprocess.run([destination/'program'],capture_output=True,text=True,timeout=60,env=os.environ|{'KALANDRA_WORKERS':str(workers)})
            assert not any(t in ran.stderr for t in ('AddressSanitizer','ThreadSanitizer','runtime error:')),ran.stderr
            if fault:
                assert ran.returncode!=0 and ran.stdout=='' and f'Metal task failed: 1/{fault}' in ran.stderr,(path,ran)
            else:
                assert ran.returncode==0 and ran.stdout==output+'\n' and all(line.endswith(('Metal API Validation Enabled','Metal GPU Validation Enabled')) for line in ran.stderr.splitlines()),(path,ran,output)
    programs+=1

for name in names:
    check(name,'post','reduceSum(values)*scale+1',[13],rank=3,scale=2)
    check(name,'feedback','amount=reduceSum(values)\nvalues+amount',[7,8,9],rank=8,tensor=True)
    check(name,'unused-reduction','ignored=reduceSum(values)\nreduceSum(values)',[6],rank=3)
    if args.variant not in (None,'debug') and name not in ('int8','float32','float64'):continue
    check(name,'multiple','left=reduceSum(values)\nright=reduceSum(values*2)\nleft+right',[18],rank=3)
    reductions=''.join(f'r{i}=reduceSum(values)\n' for i in range(30))+'+'.join(f'r{i}' for i in range(30))
    check(name,'scalar-tier2',reductions,[30],one=True)
    choice='if scale==0:\n  reduceSum(values)\nelse:\n  7/scale'
    check(name,'choice-true',choice,[6])
    check(name,'choice-false',choice,[7],scale=1)
    different='if scale==0:\n  values+1\nelse:\n  other+2'
    check(name,'different-true',different,[2,3,4],extra=True,tensor=True)
    check(name,'different-false',different,[6,7],extra=True,tensor=True,scale=1)
    shape='if scale==0:\n  0\nelse:\n  exact(values)'
    check(name,'helper-skipped',shape,[0],helper=True)
    check(name,'helper-selected',shape,[],helper=True,scale=1,fault=1)
    local='if scale==0:\n  values\nelse:\n  values+other'
    check(name,'shape-skipped',local,[1,2,3],extra=True,tensor=True)
    check(name,'shape-selected',local,[],extra=True,tensor=True,scale=1,fault=1)
    check(name,'mixed-conversion','reduceSum(float64(values))*2',[12],rank=3,target='float64')
    if not name.startswith('float'):
        check(name,'empty-scalar-fault','amount=7/reduceSum(values)\nvalues+amount',[],rank=8,empty=True,tensor=True,fault=4)
        check(name,'unused-fault','ignored=7/scale\nreduceSum(values)',[],fault=4)
    else:
        check(name,'empty-feedback','amount=7/reduceSum(values)\nvalues+amount',[],rank=8,empty=True,tensor=True)
print(f'Typed numerical programs: {programs} source cases, resident sums/feedback, selected primitives, conditional shapes, helpers, mixed conversions and checked faults passed')
