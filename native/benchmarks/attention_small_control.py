"""Independent five-block replication of small-case losses, with a large-case control."""
import argparse,json,os,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'));import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
for arg in ('baseline','evidence','output'):parser.add_argument('--'+arg,type=Path,required=True)
args=parser.parse_args();assert not args.output.exists()
for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES','KALANDRA_GPU_PROFILE'):assert os.getenv(flag) in (None,'','0'),flag
base=args.baseline.resolve();data=json.loads(args.evidence.read_text())
accept=next(p.ROOT/f for f in data['artifact_sha256'] if f.endswith('/TransformerProgram.h')).parent
bundle=accept/'bundle';old=p.ROOT/'benchmarks/.build/gpu-dispatch-2026-10-28/acceptance/bundle'
binaries={'baseline':base/'gpu','current':accept/'gpu'};bundles={'baseline':old,'current':bundle}
manifest=json.loads((base/'manifest.json').read_text())
assert p.sha(base/'gpu')==manifest['gpu']['sha256']
for f in [accept/'gpu',*bundle.glob('*.metallib')]:assert p.sha(f)==data['artifact_sha256'][p.key(f)]
shapes=[(1,64),(1,256),(1,512),(64,512)];verification=[]
for m,d in shapes:
 a,b=[json.loads(p.run([binaries[v],bundles[v],m,d,1,1,0,1,1])) for v in binaries]
 assert a==b;verification.append(dict(m=m,d=d,bit_identical=True))
def invoke(cell,count,rounds):
 v,m,d,mode=cell
 return p.observations(p.run([binaries[v],bundles[v],m,d,count,rounds,mode,0,0]),m*d*4,count,rounds)
cells=[(v,m,d,mode) for m,d in shapes for mode in range(3) for v in binaries]
result=p.measure_paired(cells,invoke,lambda c:c[1]*c[2]*4,blocks=5,target_ms=30,adaptive=True,max_iterations=5000)
result.update(contract=__doc__,verification=verification,acceptance_sha256=p.sha(args.evidence))
p.evidence(args.output,result,[Path(__file__),args.evidence,base/'manifest.json'],[*binaries.values(),*bundle.glob('*.metallib'),*old.glob('*.metallib')],p.ROOT/'build/native/native/kalandra')
