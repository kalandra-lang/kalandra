"""Prepared typed programs: immutable plans, changing captures and bounded ACK ownership."""
import argparse,json,os,sys,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'native/benchmarks/fair'))
import prepared_program
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler',type=Path,required=True)
parser.add_argument('--clang',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--quick',action='store_true')
args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
compiler=args.compiler.resolve();clang=args.clang.absolute();native=compiler.parent

def run(command):
    r=subprocess.run(list(map(str,command)),text=True,capture_output=True,timeout=360)
    assert r.returncode==0,(command,r.returncode,r.stdout,r.stderr)
    assert not any(x in r.stderr for x in ('AddressSanitizer','ThreadSanitizer','runtime error:')),r.stderr
    return r.stdout
source=out/'model.k'
source.write_text((root/'native/benchmarks/TransformerBlock.k').read_text().split('fn main(')[0]+'''fn main()->Tensor[float32,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    x:Tensor[float32,2,2]=tensor([[1,0],[0,1]])
    up:Tensor[float32,2,8]=tensor([[1,0,1,0,1,0,1,0],[0,1,0,1,0,1,0,1]])
    down:Tensor[float32,8,2]=tensor([[1,0],[0,1],[1,0],[0,1],[1,0],[0,1],[1,0],[0,1]])
    a=try await transfer(x,gpu)
    b=try await transfer(up,gpu)
    c=try await transfer(down,gpu)
    y=try await spawn block(a,a,a,a,a,b,c,0.00001,0.70710677) on gpu
    try await transfer(y,devices.cpu())
''')
bundle=out/'bundle';run([compiler,source,'--target','metal','--arithmetic','hardware','-O2','-o',bundle])
host=(bundle/'host.cpp').read_text()
assert len(__import__('re').findall(r'NumericInstructionKind::Map,[^}]*,1u,\d+u\}',host))==4
assert host.count(',0u,8u}') == 4 # structural transform work proofs
assert host.count(',0u,2u}') == 8 # hardware matrix work proofs
(out/'TransformerProgram.h').write_text(prepared_program.header(host))
(out/'LegacyProgram.h').write_text(prepared_program.header(host,False))
variants=[('optimized',['-O2'],'libkalandra-task-core-optimized.a')]
if not args.quick:variants += [('debug',['-O0'],'libkalandra-native-runtime.a'),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'],'libkalandra-native-runtime-sanitized.a'),('thread-sanitized',['-O1','-g','-fsanitize=thread','-fno-omit-frame-pointer'],'libkalandra-native-runtime-thread-sanitized.a')]
unsafe=out/'checked-cast.k'
unsafe.write_text('''fn work(x:Tensor[float32,?,?])->Tensor[float32,?,?]:
  normal=rmsNormRows(x,0.00001)
  checked=float32(int32(normal*1.0e30))
  softmaxRows(checked)
fn main()->Tensor[float32,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    x:Tensor[float32,2,2]=tensor([[1,2],[3,4]])
    a=try await transfer(x,gpu)
    y=try await spawn work(a) on gpu
    try await transfer(y,devices.cpu())
''')
residual=out/'residual.k'
residual.write_text('''fn work(a:Tensor[float32,?,?],b:Tensor[float32,?,?],c:Tensor[float32,?,?])->Tensor[float32,?,?]:
  c-matmul(transpose(a),b)
fn main()->Tensor[float32,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    a:Tensor[float32,2,2]=tensor([[1,3],[2,4]])
    b:Tensor[float32,2,2]=tensor([[2,0],[0,2]])
    c:Tensor[float32,2,2]=tensor([[10,20],[30,40]])
    x=try await transfer(a,gpu)
    y=try await transfer(b,gpu)
    z=try await transfer(c,gpu)
    w=try await spawn work(x,y,z) on gpu
    try await transfer(w,devices.cpu())
''')
residual_bundle=out/'residual-bundle'
run([compiler,residual,'--target','metal','--arithmetic','hardware','-O2','-o',residual_bundle])
assert run([residual_bundle/'program']).strip()=='[41000000,41800000,41c00000,42000000]'
records=[]
for name,flags,task in variants:
    binary=out/name
    run([clang,'-std=c++23','-pthread','-ffp-contract=off',*flags,'-I'+str(out),root/'native/tests/PreparedNumericProgramTests.cpp',native/'metal'/f'libkalandra-metal-{name}.a',native/task,'-framework','Foundation','-framework','Metal','-o',binary])
    result=run([binary,bundle,residual_bundle]);print(name+': '+result.strip(),flush=True);records.append(dict(mode=name,result=result))
    checked=out/('checked-'+name)
    flag={'optimized':'-O2','debug':'-O0','sanitized':'--sanitize','thread-sanitized':'--thread-sanitize'}[name]
    # Run the actual compiler-generated source entry through its Task overload,
    # in addition to the per-cell production-API drivers above.
    model_bundle=bundle if name=='optimized' else out/('source-model-'+name)
    if name!='optimized':run([compiler,source,'--target','metal','--arithmetic','hardware',flag,'-o',model_bundle])
    run([model_bundle/'program'])
    run([compiler,unsafe,'--target','metal','--arithmetic','hardware',flag,'-o',checked])
    generated=(checked/'host.cpp').read_text()
    assert len(__import__('re').findall(r'NumericInstructionKind::Map,[^}]*,0u(?:,8u)?\}',generated))>=3
    failed=subprocess.run([str(checked/'program')],capture_output=True,text=True,timeout=60)
    assert failed.returncode and 'Metal task failed: 1/4' in failed.stderr,failed
    assert not any(x in failed.stderr for x in ('AddressSanitizer','ThreadSanitizer','runtime error:')),failed.stderr

(out/'qualification.json').write_text(json.dumps(dict(contract=__doc__,records=records),indent=2)+'\n')
