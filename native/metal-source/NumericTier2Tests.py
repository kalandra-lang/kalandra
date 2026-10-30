"""Typed Tier 2 maps and GPU-only CPU-projection boundaries."""
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
for name,width in ((n,w) for n,w in zip(names,widths)):
  for case in ('values','empty','empty-fault'):
    if case=='empty-fault' and name.startswith('float'):continue
    empty=case!='values'
    params=','.join(f'a{i}:Tensor[{name},?]' for i in range(30))
    expression='+'.join(f'a{i}' for i in range(30))
    body=f'{name}({expression})' if case!='empty-fault' else f'amount=7/scale\n  {name}({expression})+amount'
    params+=f',scale:{name}'
    text=f'''fn operation({params})->Tensor[{name},?]:
  {body}
fn increment(value:int32)->int32:
  value+1
fn main()->Tensor[{name},?] ! TaskError:
  try parallel:
    cpu=spawn increment(0)
    gpu=devices.gpu()
    values:Tensor[{name},{0 if empty else 1}]=tensor({'[]' if empty else '[1]'})
    resident=try await transfer(values,gpu)
    answer=try await spawn operation({','.join(['resident']*30)},0) on gpu
    returned=try await transfer(answer,devices.cpu())
    amount=try await cpu
    returned+{name}(amount-1)
'''
    path,destination=args.root/f'{name}-{case}.k',args.root/f'{name}-{case}';path.write_text(text)
    built=subprocess.run(list(map(str,[args.compiler,"--arithmetic","software",path,*flags,'-o',destination])),capture_output=True,text=True,timeout=120)
    assert built.returncode==0,(name,built.stdout,built.stderr)
    manifest=(destination/'manifest.txt').read_text()
    assert 'binding_layout=argument-buffer-tier2' in manifest,manifest
    assert '// kalandra.numeric.indirect' in (destination/'kernel0.mlir').read_text()
    # The GPU-only 30-input function must not be lowered into a CPU body or
    # exported wrapper. Used CPU exports retain their original ordinals.
    cpu=(destination/'cpu.ll').read_text()
    assert '@kalandra_fn_0(' not in cpu and '@_mlir_ciface_kalandra_cpu_0(' not in cpu
    assert '@_mlir_ciface_kalandra_cpu_1(' in cpu
    if args.variant:
        bits=int.from_bytes(struct.pack('<f' if width==32 else '<d',30),'little') if name.startswith('float') else 30
        expected='[]\n' if empty else f'[{bits:0{width//4}x}]\n'
        for workers in (1,2,4):
            ran=subprocess.run([destination/'program'],capture_output=True,text=True,timeout=60,env=os.environ|{'KALANDRA_WORKERS':str(workers)})
            if case=='empty-fault':
                assert ran.returncode!=0 and ran.stdout=='' and 'Metal task failed: 1/4' in ran.stderr,(name,case,ran)
            else:
                assert ran.returncode==0 and ran.stdout==expected and all(line.endswith('Metal API Validation Enabled') for line in ran.stderr.splitlines()),(name,case,ran,expected)
print('Typed Tier 2: ten types, 30 live tensor captures, empty domains and uniform faults, exact results, mixed CPU workers and sparse CPU export ordinals passed')
