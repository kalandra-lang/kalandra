"""Profile frozen work-batching artifacts separately; validate dispatch and submission counts."""
import argparse,json,os,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
for name in ('acceptance-dir','program-dir','build-dir','output'):
    parser.add_argument('--'+name,type=Path,required=True)
args=parser.parse_args(); assert not args.output.exists()
accept=args.acceptance_dir.resolve();program=args.program_dir.resolve();bundle=program/'bundle'
saved=json.loads((accept/'manifest.json').read_text())
for f,digest in saved['artifacts'].items():assert p.sha(p.ROOT/f)==digest
for f,digest in saved['signature'].items():assert p.sha(p.ROOT/f)==digest
for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):
    assert os.getenv(flag) in (None,'','0'),flag
build=args.build_dir.resolve();build.mkdir(parents=True,exist_ok=False)
driver=build/'Profile.cpp';text=(accept/'Driver.cpp').read_text()
assert text.count('auto made=m::Runtime::create(options);')==1
text=text.replace('auto made=m::Runtime::create(options);','options.profile=true; auto made=m::Runtime::create(options);')
driver.write_text(text)
fair=Path(__file__).resolve().parent/'fair';binary=build/'profile'
p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0',
       '-I'+str(accept),'-I'+str(fair),driver,accept/'metal.a',accept/'task.a',
       '-framework','Foundation','-framework','Metal','-o',binary])
rows=[]
for m,d in [(m,d) for d in (64,256,512) for m in (1,16,64)]:
    for mode in range(3):
        for current in (False,True):
            for process in range(2):
                count=3
                samples=p.observations(p.run([binary,bundle,m,d,count,3,mode,0,0,1,int(current)]),m*d*4,count,3)
                expected=(5 if (m,d)==(64,512) else 3 if (m,d) in ((16,512),(64,256)) else 1) if current else (6 if (m,d)==(64,512) else 5 if (m,d) in ((16,512),(64,256)) else 4)
                for sample in samples:
                    assert sample['gpu_ns']>0 and sample['encoding_ns']>0
                    assert sample['dispatches']==12*count and sample['submissions']==expected*count,(m,d,mode,current,sample)
                rows.append(dict(m=m,d=d,mode=mode,current=current,process=process,iterations=count,samples=samples))
    print(f'{m}x{d}: both policies, all residency modes, 12 dispatches verified',flush=True)
p.evidence(args.output,dict(contract=__doc__,observations=rows,
    note='Instrumented diagnostic only. The sole driver change is profile=true; runtime archives, shaders, input copies and arithmetic remain frozen. Encoding counters omit some host preparation and are not an exhaustive exclusive-time breakdown.'),
    [Path(__file__),driver,accept/'Driver.cpp',accept/'TransformerProgram.h',accept/'manifest.json'],
    [binary,accept/'metal.a',accept/'task.a',*bundle.glob('*.metallib')],p.ROOT/'build/native/native/kalandra')
