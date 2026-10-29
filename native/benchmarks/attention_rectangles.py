"""Isolated contraction-parallel attention policy: general matrix validation and timing."""
import argparse,json,os,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'));import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
for flag in ('baseline','shader','build-dir','output'):parser.add_argument('--'+flag,type=Path,required=True)
parser.add_argument('--current-metal',type=Path,required=True);parser.add_argument('--layout',type=Path,required=True)
parser.add_argument('--blocks',type=int,default=3);args=parser.parse_args();assert not args.output.exists()
for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION'):assert os.getenv(flag) in (None,'','0')
baseline=args.baseline.resolve();build=args.build_dir.resolve();build.mkdir(parents=True,exist_ok=False);root=p.ROOT;fair=Path(__file__).parent/'fair'
metal=args.current_metal.resolve();task=root/'build/native/native/libkalandra-task-core-optimized.a'
binaries={};sources=[Path(__file__),fair/'MatmulPolicy.cpp',fair/'ResidualFfn.h',args.layout.resolve()]
for version,archive in [('baseline',baseline/'metal.a'),('current',metal)]:
 binary=build/version;binaries[version]=binary
 p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off',fair/'MatmulPolicy.cpp',archive,baseline/'task.a','-framework','Metal','-framework','Foundation','-o',binary])
shapes=[(4,256,8),(8,256,16),(16,256,16),(64,256,64),(16,512,16),(64,512,64),(17,544,17),(32,1024,32),(64,2048,64),(3,4096,8),(64,4096,64),(2,512,16),(65,512,64),(16,512,65),(16,224,16),(16,288,16)]
verification=[];phase_cache={}
for m,k,n in shapes:
 key=(k,n)
 if key not in phase_cache:phase_cache[key]=[[sum(((left+i)%17-8)*((right+i*n)%13-6) for i in range(k))/64 for right in range(13)] for left in range(17)]
 expected=[phase_cache[key][(row*k)%17][col%13] for row in range(m) for col in range(n)]
 for version,binary in binaries.items():
  result=json.loads(p.run([binary,args.shader,m,k,n,1,1,1,0]));assert result['cells']==expected,(version,m,k,n)
 verification.append(dict(m=m,k=k,n=n,cells=m*n,exact_dyadic_oracle=True))
def invoke(cell,count,rounds):
 version,m,k,n=cell
 return p.observations(p.run([binaries[version],args.shader,m,k,n,count,rounds,0,0]),m*n*4,count,rounds)
cells=[(version,*shape) for shape in shapes for version in binaries]
data=p.measure_paired(cells,invoke,lambda c:c[1]*c[3]*4,blocks=args.blocks,target_ms=20,adaptive=True,max_iterations=5000)
data.update(contract=__doc__,verification=verification,numerical_note='All cells checked outside timing against exact integer contractions divided by 64. Measured calls observe result metadata, not device-to-host payload.')
p.evidence(args.output,data,sources,[*binaries.values(),metal,baseline/'metal.a',baseline/'task.a',args.shader],root/'build/native/native/kalandra')
