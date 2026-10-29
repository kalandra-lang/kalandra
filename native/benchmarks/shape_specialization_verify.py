"""Full-output screening for a copied bounded shape-specialization prototype.

This is not fault-injection, resource-bound, cancellation or production acceptance.
"""
import argparse,json,os,struct,subprocess,sys
from pathlib import Path
os.environ['OPENBLAS_NUM_THREADS']='1';os.environ['VECLIB_MAXIMUM_THREADS']='1'
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import transformer_reference as ref
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--build',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);a=parser.parse_args();assert not a.output.exists()
build=a.build.resolve();manifest=build/'manifest.json';saved=json.loads(manifest.read_text())
for field in ('signature','artifacts'):
 for file,h in saved[field].items():assert p.sha(p.ROOT/file)==h,file
baseline=p.ROOT/'benchmarks/.build/gpu-closed-entry-2026-10-29/acceptance/gpu'
bundle=p.ROOT/'benchmarks/.build/gpu-attention-2026-10-29/acceptance/bundle'
def invoke(binary,m,d,mode,pattern,policy,trace):
 command=list(map(str,[binary,(build/'bundle' if binary==build/'gpu' else bundle),m,d,1,1,mode,1,pattern,policy,1]));p.COMMANDS.append(command)
 env=dict(os.environ,KALANDRA_QKV_CONCURRENT='1',MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')
 if trace:env['KALANDRA_QKV_TRACE']='1'
 result=subprocess.run(command,capture_output=True,text=True,env=env,timeout=180)
 assert result.returncode==0,(command,result.returncode,result.stderr)
 row=json.loads(result.stdout);groups=[line for line in result.stderr.splitlines() if line.startswith('QKV_GROUP ')]
 return row,groups
checks=[]
shapes=[(2,3,1),(3,7,1),(5,17,1),(17,33,1)]+[(m,d,0) for d in (64,256,512) for m in (1,16,64)]
for m,d,pattern in shapes:
 before,_=invoke(baseline,m,d,0,pattern,2,False)
 reference_bits=struct.pack('<'+'f'*len(before['cells']),*before['cells'])
 for policy in (0,1,2):
  for mode in (0,1,2):
   row,groups=invoke(build/'gpu',m,d,mode,pattern,policy,True)
   assert struct.pack('<'+'f'*len(row['cells']),*row['cells'])==reference_bits,(m,d,policy,mode)
   assert groups==[],groups
   metrics,_=ref.assess(m,d,pattern,row)
   checks.append(dict(m=m,d=d,pattern=pattern,policy=policy,mode=mode,bit_identical=True,groups=groups,oracle=metrics))
 print(f'{m}x{d}: all policies/uploads exact; generic/fused shape paths verified',flush=True)
for field in ('signature','artifacts'):
 for file,h in saved[field].items():assert p.sha(p.ROOT/file)==h,file
p.evidence(a.output,dict(contract=__doc__,production_change=False,checks=checks,validation=True,larger_shape_budget_evidence='benchmarks/results/gpu-independent-dispatch-budget-2026-10-29.json'),[Path(__file__),manifest,*[p.ROOT/f for f in saved['signature']]], [baseline,*[p.ROOT/f for f in saved['artifacts']],*bundle.glob('*.metallib')],p.ROOT/'build/native/native/kalandra')
