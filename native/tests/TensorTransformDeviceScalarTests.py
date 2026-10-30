"""Source-level RMS epsilon computed on the device, including first arithmetic failure."""
import argparse
from pathlib import Path
import struct
import subprocess
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--quick',action='store_true')
args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
def run(command,success=True):
    result=subprocess.run(list(map(str,command)),text=True,capture_output=True,timeout=180)
    assert (result.returncode==0)==success,(command,result.returncode,result.stdout,result.stderr)
    return result
valid='''fn work(x:Tensor[float32,?,?])->Tensor[float32,?,?]:
  rmsNormRows(x,reduceSum(x*x)+0.00001)
'''
cpu='''fn main()->float32:
  x:Tensor[float32,2,3]=tensor([[1,2,3],[4,5,6]])
  reduceSum(work(x))
'''
gpu='''fn main()->float32 ! TaskError:
  try parallel:
    gpu=devices.gpu()
    cpu=devices.cpu()
    x:Tensor[float32,2,3]=tensor([[1,2,3],[4,5,6]])
    a=try await transfer(x,gpu)
    b=try await spawn work(a) on gpu
    result=try await transfer(b,cpu)
    reduceSum(result)
'''
cpu_source=args.output/'cpu.k';cpu_source.write_text(valid+cpu)
run([args.compiler,cpu_source,'--arithmetic','hardware','-O2','-o',args.output/'cpu'])
value=float(run([args.output/'cpu']).stdout.strip())
expected=struct.unpack('<I',struct.pack('<f',value))[0]
variants=[('optimized','-O2')]
if not args.quick:variants += [('debug','-O0'),('sanitized','--sanitize'),('thread-sanitized','--thread-sanitize')]
for name,flag in variants:
    for bad in (False,True):
        definition=valid.replace('reduceSum(x*x)+0.00001','-reduceSum(x*x)') if bad else valid
        path=args.output/f'{name}-{bad}.k';path.write_text(definition+gpu)
        out=args.output/f'{name}-{bad}'
        run([args.compiler,path,'--target','metal','--arithmetic','hardware',flag,'-o',out])
        assert 'NumericType::Float32,true)' in (out/'host.cpp').read_text()
        result=run([out/'program'],not bad)
        if bad:assert 'Metal task failed: 1/4' in result.stderr,result.stderr
        else:assert int(result.stdout.strip(),16)==expected,(name,expected,result.stdout)
    print(name+': source-computed device epsilon, scalar-buffer roles and arithmetic failure passed',flush=True)
# Reject unsupported row arithmetic explicitly; transpose remains available.
for numeric,mode in [('float64','hardware'),('float32','software')]:
    path=args.output/f'rejected-{numeric}-{mode}.k';path.write_text((valid.replace('reduceSum(x*x)+0.00001','0.00001')+gpu).replace('float32',numeric))
    result=run([args.compiler,path,'--target','metal','--arithmetic',mode,'-O2','-o',args.output/'rejected'],False)
    assert 'error[G001]' in result.stderr and ('float32' in result.stderr or 'no hardware float64 arithmetic' in result.stderr),result.stderr
