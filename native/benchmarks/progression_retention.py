"""Isolate retained versus lease-owned Metal references in the same prepared command chain."""
import argparse,json,os,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--evidence',type=Path,required=True)
parser.add_argument('--build-dir',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--blocks',type=int,default=5)
args=parser.parse_args();assert not args.output.exists()
for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES','KALANDRA_GPU_PROFILE'):assert os.getenv(flag) in (None,'','0'),flag
data=json.loads(args.evidence.read_text());root=p.ROOT
headers=[root/f for f in data['artifact_sha256'] if f.endswith('/TransformerProgram.h')];assert len(headers)==1
frozen=headers[0].parent;bundle=frozen/'bundle';build=args.build_dir.resolve();build.mkdir(parents=True,exist_ok=False)
metal=root/'build/native/native/metal/libkalandra-metal-optimized.a';task=root/'build/native/native/libkalandra-task-core-optimized.a';fair=Path(__file__).parent/'fair'
for f in [metal,task,frozen/'gpu',*bundle.glob('*.metallib')]:assert p.sha(f)==data['artifact_sha256'][p.key(f)],f
text=(fair/'TransformerGpu.cpp').read_text();assert text.count('m::Runtime::create()')==1
text=text.replace('m::Runtime::create()','m::Runtime::create({.preencodedProgression=true,.retainedProgressionReferences=true})').replace('../../metal/Metal.h',str(root/'native/metal/Metal.h'))
cpp=build/'Retained.cpp';cpp.write_text(text)
p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-I'+str(fair.resolve()),'-I'+str(frozen),cpp,metal,task,'-framework','Foundation','-framework','Metal','-o',build/'retained'])
binaries={'baseline':frozen/'gpu','current':build/'retained'}
verification=[]
shapes=[(m,d) for d in (64,256,512) for m in (1,16,64)]
for m,d in shapes:
    results=[json.loads(p.run([binary,bundle,m,d,1,1,0,1,1])) for binary in binaries.values()]
    bits=[struct.pack('<'+'f'*len(r['cells']),*r['cells']) for r in results]
    assert bits[0]==bits[1]
    verification.append(dict(m=m,d=d,cells=m*d,bit_identical=True))
def invoke(cell,count,rounds):
    version,m,d,mode=cell
    return p.observations(p.run([binaries[version],bundle,m,d,count,rounds,mode,0,0]),m*d*4,count,rounds)
cells=[(version,m,d,mode) for m,d in shapes for mode in range(3) for version in binaries]
result=p.measure_paired(cells,invoke,lambda c:c[1]*c[2]*4,blocks=args.blocks,target_ms=20,adaptive=True,max_iterations=5000)
result.update(contract=__doc__,control='baseline = unretained references; current = retained references; same compiled runtime, shader libraries, and command progression',verification=verification,acceptance_sha256=p.sha(args.evidence))
p.evidence(args.output,result,[Path(__file__),cpp,args.evidence,fair/'TransformerGpu.cpp',frozen/'TransformerProgram.h'],[*binaries.values(),metal,task,*bundle.glob('*.metallib')],root/'build/native/native/kalandra')
