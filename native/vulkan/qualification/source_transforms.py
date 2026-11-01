"""Compile and execute complete Vulkan transform tasks against independent references.

Runs selected physical ICDs at 1/2/4 workers, including repeated awaits and an
explicit CPU download. Private qualification adapter only; installs nothing.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--compile-only',action='store_true',help='Hosted artifact qualification; never execute a substitute GPU')
a=p.parse_args();a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=True)
repo=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(repo/'native/tests'))
from TensorTransformReference import D,to_bits,from_bits
from TransformReference import reference

def run(command,**kwargs):
    return subprocess.run(list(map(str,command)),text=True,capture_output=True,timeout=180,**kwargs)

types=[('float32','f','I'),('float64','d','Q')]+[(('u' if u else '')+'int'+str(w),'','B' if w==8 else 'H' if w==16 else 'I' if w==32 else 'Q') for w in (8,16,32,64) for u in (False,True)]
cases=[]
for name,fmt,wire in types:
    for mode in ('hardware','software'):
        cases.append((name,fmt,wire,mode,'transpose','transpose(x)',0,None,False))
for kind,body,epsilon,fault in [('softmaxRows','softmaxRows(x)',None,False),
                              ('rmsNormRows','rmsNormRows(x,0.00001)',D.from_float(from_bits(to_bits(.00001,'f','I'),'f','I')),False),
                              ('rms-device','rmsNormRows(x,reduceSum(x*x))',D(91),False),
                              ('rms-invalid','rmsNormRows(x,-reduceSum(x*x))',None,True)]:
    cases.append(('float32','f','I','hardware',kind,body,1 if kind=='softmaxRows' else 2,epsilon,fault))
for name,fmt,wire,mode,kind,body,operation,epsilon,fault in cases:
    case=a.output/f'{name}-{kind}-{mode}';case.mkdir(exist_ok=True)
    source=case/'source.k'
    source.write_text(f'''fn work(x:Tensor[{name},?,?])->Tensor[{name},?,?]:
  {body}
fn main()->Tensor[{name},?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    cpu=devices.cpu()
    x:Tensor[{name},2,3]=tensor([[1,2,3],[4,5,6]])
    a=try await transfer(x,gpu)
    pending=spawn work(a) on gpu
    first=try await pending
    again=try await pending
    try await transfer(again,cpu)
''')
    compiled=run([sys.executable,Path(__file__).with_name('compiler_driver.py'),source,'--arithmetic',mode,'-O2','-o',case])
    (case/'compile.log').write_text(compiled.stdout+compiled.stderr)
    assert compiled.returncode==0,(case,compiled.stderr[-3000:])
    if a.compile_only:
        print('PASS:',case.name,'compiled source artifacts only',flush=True)
        continue
    inputs=[to_bits(x,fmt,wire) for x in range(1,7)] if fmt else list(range(1,7))
    expected=reference(inputs,2,3,operation,epsilon,fmt,wire) if not fault else None
    for label,icd in [('nvidia','/usr/share/vulkan/icd.d/nvidia_icd.x86_64.json'),('intel','/usr/share/vulkan/icd.d/intel_icd.x86_64.json')]:
        for workers in (1,2,4):
            got=run([case/'program'],env=os.environ|{'KALANDRA_WORKERS':str(workers),'VK_DRIVER_FILES':icd})
            (case/f'{label}-{workers}.json').write_text(json.dumps({'status':got.returncode,'stdout':got.stdout,'stderr':got.stderr},indent=2)+'\n')
            if fault:
                assert got.returncode and not got.stdout and 'Vulkan task failed: 1/4' in got.stderr,(case,label,workers,got)
                continue
            assert got.returncode==0 and not got.stderr,(case,label,workers,got)
            bits=[int(x,16) for x in got.stdout.strip().strip('[]').split(',')]
            assert len(bits)==len(expected),(case,got)
            for observed,wanted in zip(bits,expected):
                if operation==0:assert observed==wanted,(case,label,observed,wanted)
                else:
                    value=D.from_float(from_bits(observed,fmt,wire));lo,hi=wanted
                    assert lo<=value<=hi,(case,label,value,wanted)
    print('PASS:',case.name,'both GPUs workers1/2/4',flush=True)
print('PASS:',len(cases),'source artifact compilations' if a.compile_only else 'complete source programs;144 physical GPU executions',flush=True)
