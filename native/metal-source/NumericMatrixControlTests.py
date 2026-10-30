"""Selected shape checks after a resident matrix product."""
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
programs=0

def check(name,width,label,a,b,expected,total=False,fault=False,columns=None,expression="matmul(a,b)",fault_code=4):
    global programs
    m,k,n=len(a),len(b),len(b[0]) if b else columns
    literal_a=f'tensor({a})' if a else f'tensor([],[0,{k}])'
    literal_b=f'tensor({b})' if b else f'tensor([],[0,{n}])'
    result=name if total else f'Tensor[{name},?,?]'
    if total: expression='reduceSum('+expression+')'
    source=(f'fn operation(a:Tensor[{name},?,?],b:Tensor[{name},?,?])->{result}:\n  {expression}\n'
            f'fn main()->{result} ! TaskError:\n  try parallel:\n    gpu=devices.gpu()\n'
            f'    a:Tensor[{name},{m},{k}]={literal_a}\n    b:Tensor[{name},{k},{n}]={literal_b}\n'
            '    x=try await transfer(a,gpu)\n    y=try await transfer(b,gpu)\n'
            '    answer=try await spawn operation(x,y) on gpu\n'+
            ('    answer\n' if total else '    try await transfer(answer,devices.cpu())\n'))
    path,destination=args.root/f'{name}-{label}.k',args.root/f'{name}-{label}'
    path.write_text(source)
    built=subprocess.run(list(map(str,[args.compiler,"--arithmetic","software",path,*flags,'-o',destination])),capture_output=True,text=True,timeout=120)
    assert built.returncode==0,(path,built.stdout,built.stderr)
    if args.variant:
        def bits(value):
            if name.startswith('float'):
                return int.from_bytes(struct.pack('<f' if width==32 else '<d',value),'little')
            return value & ((1<<width)-1)
        words=','.join(f'{bits(v):0{width//4}x}' for v in expected)
        output=(words if total else '['+words+']')+'\n'
        for workers in (1,2,4):
            ran=subprocess.run([destination/'program'],capture_output=True,text=True,timeout=60,env=dict(os.environ,KALANDRA_WORKERS=str(workers)))
            assert not any(t in ran.stderr for t in ('AddressSanitizer','ThreadSanitizer','runtime error:')),ran.stderr
            if fault:
                assert ran.returncode!=0 and ran.stdout=='' and f'Metal task failed: 1/{fault_code}' in ran.stderr,(path,ran)
            else:
                assert ran.returncode==0 and ran.stdout==output and all(line.endswith('Metal API Validation Enabled') for line in ran.stderr.splitlines()),(path,ran,output)
    programs+=1

for name,width in (('int8',8),('uint8',8),('int16',16),('uint16',16),('int32',32),('uint32',32),('int64',64),('uint64',64),('float32',32),('float64',64)):
    a,b=[[1,2,1],[1,1,1]],[[1,1],[1,1],[1,1]]
    check(name,width,'post-skipped-shape',a,b,[4,4,3,3],expression='product=matmul(a,b)\n  if reduceSum(product)>0:\n    product\n  else:\n    product+a')
    check(name,width,'post-selected-shape',a,b,[],expression='product=matmul(a,b)\n  if reduceSum(product)==0:\n    product\n  else:\n    product+a',fault=True,fault_code=1)
print(f'Typed matrix control: {programs} source cases, ten numeric types, selected post-product shape checks passed')
