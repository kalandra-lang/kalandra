"""Exact heterogeneous source maps/scalars and checked empty conversion faults."""
import argparse
import os
from pathlib import Path
import struct
import subprocess

parser=argparse.ArgumentParser()
parser.add_argument('--compiler',required=True)
parser.add_argument('--root',type=Path,required=True)
parser.add_argument('--variant',choices=('debug','optimized','sanitized','thread-sanitized'))
args=parser.parse_args()
args.root.mkdir(parents=True,exist_ok=True)
flags=[] if not args.variant else ['--target','metal',{'debug':'-O0','optimized':'-O2','sanitized':'--sanitize','thread-sanitized':'--thread-sanitize'}[args.variant]]
names=('int8','uint8','int16','uint16','int32','uint32','int64','uint64','float32','float64')
widths=(8,8,16,16,32,32,64,64,32,64)
programs=0

def check(source,target,kind='map',fault=False):
    global programs
    width=widths[names.index(target)]
    scalar=kind=='scalar'
    empty=kind=='empty'
    rank=8 if target=='float64' and not scalar else 1
    shape=','.join(['?']*rank)
    argument=f'value:{source}' if scalar else f'values:Tensor[{source},{shape}],extra:{target}'
    result=target if scalar else f'Tensor[{target},{shape}]'
    body=f'{target}(value)+1' if scalar else f'{target}(values)+extra'
    if fault:
        argument=f'values:Tensor[{source},?],extra:{source}'
        body=f'amount={target}(extra)\n{target}(values)+amount'
    if scalar:
        construction=''
        arguments='3'
    else:
        literal='[]' if empty else '[1,2,3]'
        for _ in range(rank-1):literal='['+literal+']'
        dimensions=','.join(['1']*(rank-1)+['0' if empty else '3'])
        construction=(f'    values:Tensor[{source},{dimensions}]=tensor({literal})\n'
                      '    resident=try await transfer(values,gpu)\n')
        arguments='resident,'+('1e40' if fault else '1')
    text=(f'fn operation({argument})->{result}:\n'+''.join('  '+line+'\n' for line in body.splitlines())+
          f'fn main()->{result} ! TaskError:\n  try parallel:\n    gpu=devices.gpu()\n'+construction+
          f'    work=spawn operation({arguments}) on gpu\n    answer=try await work\n'+
          ('    answer\n' if scalar else '    try await transfer(answer,devices.cpu())\n'))
    path,destination=args.root/f'{source}-{target}-{kind}.k',args.root/f'{source}-{target}-{kind}'
    path.write_text(text)
    built=subprocess.run(list(map(str,[args.compiler,"--arithmetic","software",path,*flags,'-o',destination])),capture_output=True,text=True,timeout=120)
    assert built.returncode==0,(path,built.returncode,built.stdout,built.stderr)
    if args.variant:
        def bits(value):
            return int.from_bytes(struct.pack('<f' if width==32 else '<d',value),'little') if target.startswith('float') else value
        values=[] if empty else [4] if scalar else [2,3,4]
        words=','.join(f'{bits(v):0{width//4}x}' for v in values)
        output=(words if scalar else '['+words+']')+'\n'
        for workers in (1,2,4):
            ran=subprocess.run([destination/'program'],capture_output=True,text=True,timeout=60,env=dict(os.environ,KALANDRA_WORKERS=str(workers)))
            assert not any(t in ran.stderr for t in ('AddressSanitizer','ThreadSanitizer','runtime error:')),ran.stderr
            if fault:
                assert ran.returncode!=0 and ran.stdout=='' and 'Metal task failed: 1/4' in ran.stderr,(path,ran)
            else:
                assert ran.returncode==0 and ran.stdout==output and all(line.endswith('Metal API Validation Enabled') for line in ran.stderr.splitlines()),(path,ran,output)
    programs+=1

for source in names:
    for target in names:
        # The algorithm proof independently executes every pair in all three
        # numerical modes. Full source coverage runs in debug; additional build
        # modes exercise every source/destination width through the float64 hub.
        if not args.variant or args.variant=='debug' or 'float64' in (source,target):
            check(source,target)
for source,target in zip(names,names[1:]+names[:1]):
    check(source,target,'scalar')
check('float64','int8','empty')
check('float64','int8','empty',fault=True)
print(f'Typed GPU conversions: {programs} programs, heterogeneous packed/64-bit inputs, scalar packets, rank-eight outputs and empty checked faults passed')
