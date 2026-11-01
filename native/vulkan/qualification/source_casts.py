"""Complete-source conversion/shape/task qualification for all 100 numeric pairs."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
p=argparse.ArgumentParser(description=__doc__)
for name in ('build','llvm-root','output'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--cxx',type=Path,default=Path('/usr/bin/clang++'))
p.add_argument('--compile-only',action='store_true')
p.add_argument('--relu-only',action='store_true')
p.add_argument('--quick',action='store_true',help='Identity and binary64 boundary pairs for hosted artifact qualification')
a=p.parse_args();a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=True)
types=('int8','uint8','int16','uint16','int32','uint32','int64','uint64','float32','float64')
checks=0;programs=0

def qualify(name,source,mode,values,destination,fault=0,disable_native_float64=False):
    global checks,programs
    output=a.output/(name+'-'+mode);output.mkdir(exist_ok=True)
    path=output/'source.k';path.write_text(source)
    command=[sys.executable,Path(__file__).with_name('source_program.py'),'--build',a.build,
       '--llvm-root',a.llvm_root,'--source',path,'--output',output,'--cxx',a.cxx,'--arithmetic',mode]
    if disable_native_float64 or mode=='hardware-with-software-fallback':command.append('--test-disable-native-float64')
    built=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=180)
    (output/'compile.log').write_text(built.stdout+built.stderr)
    assert not built.returncode,(output,built.stderr[-4000:])
    programs+=1
    if a.compile_only:return
    width=32 if destination=='float32' else 64 if destination=='float64' else int(destination.removeprefix('u').removeprefix('int'))
    def bits(value):
        if destination.startswith('float'):
            return int.from_bytes(struct.pack('<f' if width==32 else '<d',float(value)),'little')
        return int(value)&((1<<width)-1)
    expected='['+','.join(f'{bits(x):0{width//4}x}' for x in values)+']\n'
    (output/'expected.json').write_text(json.dumps({'stdout':expected,'fault':fault})+'\n')
    for label,icd in [('nvidia','nvidia_icd.x86_64.json'),('intel','intel_icd.x86_64.json')]:
        for workers in (1,2,4):
            result=subprocess.run([output/'program'],capture_output=True,text=True,timeout=60,
                env=os.environ|{'VK_DRIVER_FILES':'/usr/share/vulkan/icd.d/'+icd,'KALANDRA_WORKERS':str(workers)})
            (output/f'{label}-{workers}.json').write_text(json.dumps({'status':result.returncode,'stdout':result.stdout,'stderr':result.stderr},indent=2)+'\n')
            if fault:assert result.returncode and not result.stdout and f'Vulkan task failed: 1/{fault}' in result.stderr,(output,result)
            else:assert not result.returncode and not result.stderr and result.stdout==expected,(output,result,expected)
            checks+=1

if not a.relu_only:
    for source in types:
        for destination in types:
            if a.quick and source!=destination and 'float64' not in (source,destination):continue
            values=[1,2,3] if source.startswith('u') or destination.startswith('u') else [-3,0,5]
            if source.startswith('float'):values=[1.25,2.5,3.75] if destination.startswith('u') else [-3.25,.5,5.75]
            text=f'''fn work(x:Tensor[{source},?,?,?])->Tensor[{destination},?,?,?]:
  {destination}(x)
fn main()->Tensor[{destination},?,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    x:Tensor[{source},1,1,3]=tensor([[{values}]])
    resident=try await transfer(x,gpu)
    pending=spawn work(resident) on gpu
    first=try await pending
    cached=try await pending
    try await transfer(cached,devices.cpu())
'''
            for mode in ('hardware','hardware-with-software-fallback'):
                qualify(source+'-to-'+destination,text,mode,values,destination)
        print('PASS:',source,'complete source pairs',flush=True)
    for destination in ('int8','uint64','float32'):
        text=f'''fn work(x:Tensor[float64,?])->Tensor[{destination},?]:
  {destination}(x)
fn main()->Tensor[{destination},?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    x:Tensor[float64,1]=tensor([1e300])
    resident=try await transfer(x,gpu)
    result=try await spawn work(resident) on gpu
    try await transfer(result,devices.cpu())
'''
        for mode in ('hardware','software','hardware-with-software-fallback'):
            qualify('invalid-float64-to-'+destination,text,mode,[],destination,4)
    text='''fn work(x:Tensor[float64,?,?],scale:float64)->Tensor[int8,?,?]:
  int8(x)+int8(scale)
fn main()->Tensor[int8,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    x:Tensor[float64,1,0]=tensor([[]])
    resident=try await transfer(x,gpu)
    result=try await spawn work(resident,1e300) on gpu
    try await transfer(result,devices.cpu())
'''
    for mode in ('hardware','software','hardware-with-software-fallback'):
        qualify('invalid-uniform-before-empty',text,mode,[],'int8',4)
for numeric in types:
    values=[1,2,3] if numeric.startswith('u') else [-0.0,-3.25,.5] if numeric.startswith('float') else [-3,0,5]
    expected=[x if x>0 else 0 for x in values]
    text=f'''fn work(x:Tensor[{numeric},?,?,?])->Tensor[{numeric},?,?,?]:
  relu(x)
fn main()->Tensor[{numeric},?,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    x:Tensor[{numeric},1,1,3]=tensor([[{values}]])
    resident=try await transfer(x,gpu)
    pending=spawn work(resident) on gpu
    first=try await pending
    cached=try await pending
    try await transfer(cached,devices.cpu())
'''
    for mode in ('hardware','software','hardware-with-software-fallback'):
        qualify('relu-'+numeric,text,mode,expected,numeric,disable_native_float64=True)
    print('PASS:',numeric,'complete source ReLU without float64 feature',flush=True)
print('PASS:',programs,'source programs;',checks,'physical-device source executions',flush=True)
