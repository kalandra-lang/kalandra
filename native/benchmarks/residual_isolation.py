"""Isolate residual fusion using the same reordered program/shaders and attention policy.

Also relink the old generated program against the new runtime to check ABI compatibility.
"""
import argparse,json,os,shutil,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'));import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
for arg in ('baseline','policy-metal','evidence','build-dir','output'):parser.add_argument('--'+arg,type=Path,required=True)
parser.add_argument('--blocks',type=int,default=3);args=parser.parse_args()
assert not args.output.exists()
for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES','KALANDRA_GPU_PROFILE'):assert os.getenv(flag) in (None,'','0'),flag
base=args.baseline.resolve();build=args.build_dir.resolve();build.mkdir(parents=True,exist_ok=False)
current=json.loads(args.evidence.read_text());headers=[p.ROOT/f for f in current['artifact_sha256'] if f.endswith('/TransformerProgram.h')];assert len(headers)==1
accept=headers[0].parent;bundle=accept/'bundle';fair=Path(__file__).parent/'fair';native=p.ROOT/'build/native/native'
for f in [accept/'gpu',*bundle.glob('*.metallib'),native/'metal/libkalandra-metal-optimized.a']:
 assert p.sha(f)==current['artifact_sha256'][p.key(f)]
assert p.sha(base/'task.a')==p.sha(native/'libkalandra-task-core-optimized.a')
flags=['-std=c++23','-O2','-pthread','-ffp-contract=off']
cpp=build/'Driver.cpp';cpp.write_text((base/'Driver.cpp').read_text().replace('../../metal/Metal.h',str(p.ROOT/'native/metal/Metal.h')))
p.run([p.CLANG,*flags,'-I'+str(fair.resolve()),'-I'+str(accept),cpp,args.policy_metal,base/'task.a','-framework','Foundation','-framework','Metal','-o',build/'policy-only'])
compat=build/'compat';compat.mkdir();shutil.copy2(base/'Program.h',compat/'TransformerProgram.h')
p.run([p.CLANG,*flags,'-I'+str(fair.resolve()),'-I'+str(compat),cpp,native/'metal/libkalandra-metal-optimized.a',base/'task.a','-framework','Foundation','-framework','Metal','-o',compat/'gpu'])
old_bundle=p.ROOT/'benchmarks/.build/gpu-dispatch-2026-10-28/acceptance/bundle'
binaries={'baseline':build/'policy-only','current':accept/'gpu'}
shapes=[(m,d) for d in (64,256,512) for m in (1,16,64)];verification=[]
for m,d in [(2,3),(3,7),(5,17),(17,33)]+shapes:
 outputs=[]
 for binary,kernels in [(base/'gpu',old_bundle),(compat/'gpu',old_bundle),*[(b,bundle) for b in binaries.values()]]:
  data=json.loads(p.run([binary,kernels,m,d,1,1,0,1,1]));outputs.append(struct.pack('<'+'f'*len(data['cells']),*data['cells']))
 assert all(x==outputs[0] for x in outputs),(m,d)
 verification.append(dict(m=m,d=d,cells=m*d,bit_identical=True,old_generated_program_compatible=True))
def invoke(cell,count,rounds):
 v,m,d,mode=cell
 return p.observations(p.run([binaries[v],bundle,m,d,count,rounds,mode,0,0]),m*d*4,count,rounds)
cells=[(v,m,d,mode) for m,d in shapes for mode in range(3) for v in binaries]
data=p.measure_paired(cells,invoke,lambda c:c[1]*c[2]*4,blocks=args.blocks,target_ms=20,adaptive=True,max_iterations=5000)
data.update(contract=__doc__,verification=verification,acceptance_sha256=p.sha(args.evidence),note='Only runtime fusion differs in paired timing: both binaries use the small-output attention policy, the same new generated header, and the same shader bundle. Old-header compatibility is checked separately outside timing.')
p.evidence(args.output,data,[Path(__file__),cpp,args.evidence,base/'Program.h',accept/'TransformerProgram.h'],[*binaries.values(),compat/'gpu',args.policy_metal,native/'metal/libkalandra-metal-optimized.a',base/'task.a',*bundle.glob('*.metallib'),*old_bundle.glob('*.metallib')],native/'kalandra')
