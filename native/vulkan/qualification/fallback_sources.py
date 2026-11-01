"""Qualify explicit software fallback with native float64 physically disabled.

The private source builder retains the original supervisor and compiles a
separate test host that omits shaderFloat64 from VkDeviceCreateInfo. All numerical
work still executes on the selected physical GPU. No CPU fallback/substitute.
"""
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
a=p.parse_args();a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=True)
for operation,expression in [('add','a+b'),('sub','a-b'),('mul','a*b'),('div','a/b')]:
    for mode in ('hardware','software','hardware-with-software-fallback'):
        output=a.output/(operation+'-'+mode);output.mkdir(exist_ok=True)
        source=output/'source.k'
        source.write_text(f'''fn work(a:Tensor[float64,?],b:Tensor[float64,?])->Tensor[float64,?]:
  {expression}
fn main()->Tensor[float64,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    a:Tensor[float64,3]=tensor([1,2,3])
    b:Tensor[float64,3]=tensor([4,5,6])
    x=try await transfer(a,gpu)
    y=try await transfer(b,gpu)
    pending=spawn work(x,y) on gpu
    first=try await pending
    cached=try await pending
    try await transfer(cached,devices.cpu())
''')
        command=[sys.executable,Path(__file__).with_name('source_program.py'),'--build',a.build,
            '--llvm-root',a.llvm_root,'--source',source,'--output',output,'--cxx',a.cxx,
            '--arithmetic',mode,'--test-disable-native-float64']
        built=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=180)
        (output/'compile.log').write_text(built.stdout+built.stderr)
        assert built.returncode==0,(source,built.stderr[-4000:])
        assert ('arithmetic_policy='+mode) in (output/'manifest.txt').read_text()
        if a.compile_only:
            print('PASS:',output.name,'artifact compilation only',flush=True);continue
        values=[(x+y if operation=='add' else x-y if operation=='sub' else x*y if operation=='mul' else x/y) for x,y in zip((1.,2.,3.),(4.,5.,6.))]
        expected='['+','.join(f'{struct.unpack("<Q",struct.pack("<d",x))[0]:016x}' for x in values)+']\n'
        for label,icd in [('nvidia','nvidia_icd.x86_64.json'),('intel','intel_icd.x86_64.json')]:
            for workers in (1,2,4):
                result=subprocess.run([output/'program'],capture_output=True,text=True,timeout=60,
                    env=os.environ|{'VK_DRIVER_FILES':'/usr/share/vulkan/icd.d/'+icd,'KALANDRA_WORKERS':str(workers)})
                (output/f'{label}-{workers}.json').write_text(json.dumps({'status':result.returncode,'stdout':result.stdout,'stderr':result.stderr},indent=2)+'\n')
                if mode=='hardware':
                    assert result.returncode and not result.stdout and 'Vulkan task failed: 1/106' in result.stderr,(output,result)
                else:assert not result.returncode and not result.stderr and result.stdout==expected,(output,result,expected)
        print('PASS:',output.name,'both GPUs workers1/2/4; native float64 disabled',flush=True)
print('PASS: 12 policy/source variants; '+('artifact compilation only' if a.compile_only else '72 source policy executions on physical devices; 24 expected capability rejections'),flush=True)
