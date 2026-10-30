"""Instrument frozen full-block artifacts separately from acceptance timing."""
import argparse,json,os
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--evidence',type=Path,required=True)
parser.add_argument('--build-dir',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--dispatches',type=int,default=16)
parser.add_argument('--large-submissions',type=int,default=8)
args=parser.parse_args();assert not args.output.exists()
data=json.loads(args.evidence.read_text());build=args.build_dir.resolve();build.mkdir(parents=True,exist_ok=True)
root=p.ROOT;fair=Path(__file__).parent/'fair'
headers=[Path(f) for f in data['artifact_sha256'] if f.endswith('/TransformerProgram.h')];assert len(headers)==1
frozen=(headers[0] if headers[0].is_absolute() else root/headers[0]).parent;bundle=frozen/'bundle'
metal=root/'build/native/native/metal/libkalandra-metal-optimized.a';task=root/'build/native/native/libkalandra-task-core-optimized.a';compiler=root/'build/native/native/kalandra'
sources=[Path(__file__),fair/'TransformerGpu.cpp',fair/'TransformerBlock.h',fair/'ResidualFfn.h',frozen/'TransformerProgram.h']
for f in [metal,task,frozen/'gpu',*bundle.glob('*.metallib')]:assert p.sha(f)==data['artifact_sha256'][p.key(f)],f
assert p.sha(fair/'TransformerGpu.cpp')==data['source_sha256'][p.key(fair/'TransformerGpu.cpp')]
cpp=build/'Profile.cpp'
text=(fair/'TransformerGpu.cpp').read_text();assert text.count('m::Runtime::create()')==1
text=text.replace('m::Runtime::create()','m::Runtime::create({.profile=true})').replace('../../metal/Metal.h',str(root/'native/metal/Metal.h'))
cpp.write_text(text)
binary=build/'profile'
p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-I'+str(fair.resolve()),'-I'+str(frozen),cpp,metal,task,'-framework','Foundation','-framework','Metal','-o',binary])
rows=[]
for m,d in [(1,512),(64,512)]:
    for mode in (0,1,2):
        for process in range(3):
            command=[binary,bundle,m,d,3,3,mode,0,0]
            samples=p.observations(p.run(command),m*d*4,3,3)
            for sample in samples:
                assert sample['gpu_ns']>0 and sample['encoding_ns']>0 and sample['dispatches']==args.dispatches*3
                assert sample['submissions']==((4 if m==1 else args.large_submissions)+(0,1,7)[mode])*3
            rows.append(dict(m=m,d=d,mode=mode,process=process,iterations=3,samples=samples))
        print(f'{m}x{d} mode {mode}: nonzero device/encoding counters verified',flush=True)
sources +=[args.evidence,cpp]
p.evidence(args.output,dict(contract=__doc__,note='The sole driver change is Runtime profile=true before measurement. Frozen kernels and runtime archives are identical to acceptance artifacts. Instrumented wall/device/recorded-encoding times are diagnostic; encoding counters omit some helper setup and are not an exhaustive exclusive-time breakdown.',observations=rows,acceptance_sha256=p.sha(args.evidence),source_snapshots={p.key(f):f.read_text() for f in sources if f.suffix in ('.cpp','.h','.py')}),sources,[binary,metal,task,*bundle.glob('*.metallib')],compiler)
