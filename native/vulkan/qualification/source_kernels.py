"""Build actual Kalandra typed map artifacts through the shared GPU program emitter.

This is kernel-level conformance, not a claim of a Vulkan host compiler target.
Explicit software arithmetic is selected for exact IEEE/checked numeric parity.
"""
import argparse
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser()
for option in ('compiler','lower','extract','llvm-root','validator','output'):
    p.add_argument('--'+option,type=Path,required=True)
p.add_argument('--arithmetic',choices=('hardware','software'),default='software')
p.add_argument('--operation', choices=('map', 'reduce', 'matmul', 'scalar'), default='map')
a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
types=('int8','uint8','int16','uint16','int32','uint32','int64','uint64','float32','float64')
for name in types:
    source=a.output/(name+'.k')
    if a.operation == 'map':
        source.write_text(f'''fn operation(v:Tensor[{name},?])->Tensor[{name},?]:
  v*2+3
fn main()->Tensor[{name},3] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    v:Tensor[{name},3]=tensor([1,2,3])
    resident=try await transfer(v,gpu)
    result=try await spawn operation(resident) on gpu
    try await transfer(result,devices.cpu())
''')
    elif a.operation == 'reduce':
        source.write_text(f'''fn operation(v:Tensor[{name},?])->{name}:
  reduceSum(v)
fn main()->int32:
  0
''')
    elif a.operation == 'matmul':
        source.write_text(f'''fn operation(a:Tensor[{name},?,?],b:Tensor[{name},?,?])->Tensor[{name},?,?]:
  matmul(a,b)
fn main()->int32:
  0
''')
    else:
        source.write_text(f'''fn operation(v:{name})->{name}:
  v*2+3
fn main()->int32:
  0
''')
    directory=a.output/name
    subprocess.run([str(a.compiler),str(source),'operation',str(directory),a.arithmetic],check=True)
    program=json.loads((directory/'program.json').read_text())
    assert program['arithmetic']==a.arithmetic
    if a.operation == 'map':
        assert all(op['kind']=='Map' and len(op['inputs'])==1 and not op['scalarSources'] and not op['constants'] for op in program['code'])
        assert program['result']==len(program['code'])
    elif a.operation == 'scalar':
        assert len(program['code']) == 1
        assert program['code'][0]['kind'] == 'Map'
        assert program['code'][0]['inputs'] == [] and program['code'][0]['scalarSources'] == [0]
        assert program['code'][0]['output'] == program['result'] == 0
    else:
        computes = [op for op in program['code'] if op['kind'] in ('Reduce', 'Matmul')]
        assert len(computes) == 1 and computes[0]['kind'] == a.operation.capitalize()
    for i,op in enumerate(program['code']):
        if op['kernel'] == 2**32-1:
            continue
        if a.operation == 'map':
            assert op['inputs']==[i] and op['output']==i+1 and op['kernel']==i
        base=directory/('kernel'+str(op['kernel']))
        commands=[
          [a.lower,str(base)+'.mlir',str(base)+'.lowered.mlir',a.arithmetic],
          [a.llvm_root/'bin/mlir-opt',str(base)+'.lowered.mlir','--verify-each',
           '--pass-pipeline=builtin.module(canonicalize,cse,convert-gpu-to-spirv,spirv.module(spirv-lower-abi-attrs,spirv-update-vce))','-o',str(base)+'.converted.mlir'],
          [a.extract,str(base)+'.converted.mlir',str(base)+'.kernel.mlir'],
          [a.llvm_root/'bin/mlir-translate','--no-implicit-module','--serialize-spirv',str(base)+'.kernel.mlir','-o',str(base)+'.spv'],
          [a.validator,'--target-env','vulkan1.0',str(base)+'.spv']]
        for command in commands:subprocess.run(list(map(str,command)),check=True)
    print('Compiled and validated',name,flush=True)
(a.output/'qualification.json').write_text(json.dumps({'scope':'kernel-only conformance','operation':a.operation,'arithmetic':a.arithmetic,'types':types},indent=2)+'\n')
