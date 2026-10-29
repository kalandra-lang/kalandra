"""Isolated write-combined versus default CPU cache mode for owned shared uploads."""
import argparse, io, json, os, shutil, subprocess, sys, tarfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
for name in ('acceptance-dir','program-dir','build-dir','output'):
    parser.add_argument('--'+name,type=Path,required=True)
parser.add_argument('--blocks',type=int,default=5)
args=parser.parse_args()
for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):
    assert os.getenv(flag) in (None,'','0'),flag
assert not args.output.exists()
accept=args.acceptance_dir.resolve();program=args.program_dir.resolve();bundle=program/'bundle'
manifest=json.loads((accept/'manifest.json').read_text())
for f,digest in manifest['artifacts'].items():assert p.sha(p.ROOT/f)==digest
build=args.build_dir.resolve();build.mkdir(parents=True,exist_ok=False)
source=build/'source';source.mkdir()
with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(['git','archive','dec5a66d','native'],cwd=p.ROOT))) as archive:
    archive.extractall(source,filter='data')
metal=source/'native/metal/Metal.mm'
text=(p.ROOT/'native/metal/Metal.mm').read_text()
needle='options:(staging || shared) ? MTLResourceStorageModeShared\n                                               : MTLResourceStorageModePrivate'
assert text.count(needle)==1
metal.write_text(text.replace(needle,'options:(staging || shared) ? (MTLResourceStorageModeShared |\n                        (shared ? MTLResourceCPUCacheModeWriteCombined : MTLResourceCPUCacheModeDefaultCache))\n                                               : MTLResourceStorageModePrivate'))
shutil.copy2(p.ROOT/'native/metal/Metal.h',source/'native/metal/Metal.h')
flags=['-std=c++23','-O2','-pthread','-mmacosx-version-min=26.0']
p.run([p.CLANG,*flags,'-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs','-c',metal,'-o',build/'Metal.o'])
p.run(['ar','rcs',build/'metal.a',build/'Metal.o'])
p.run([p.CLANG,*flags,'-ffp-contract=off','-I'+str(program),'-I'+str(p.ROOT/'native/benchmarks/fair'),accept/'Driver.cpp',build/'metal.a',accept/'task.a','-framework','Foundation','-framework','Metal','-o',build/'gpu'])
p.run([p.CLANG,*flags,p.ROOT/'native/metal/UploadTests.cpp',build/'metal.a',accept/'task.a','-framework','Foundation','-framework','Metal','-o',build/'upload-tests'])
qualification=p.run([build/'upload-tests'],env=os.environ|{'MTL_DEBUG_LAYER':'1','MTL_SHADER_VALIDATION':'1'})
binaries={'baseline':accept/'gpu','current':build/'gpu'}
shapes=[(1,64),(16,64),(1,256),(1,512),(64,512)]
verification=[]
for m,d in shapes:
    for mode in range(3):
        values=[json.loads(p.run([b,bundle,m,d,1,1,mode,1,1,1])) for b in binaries.values()]
        assert values[0]==values[1]
        verification.append(dict(m=m,d=d,mode=mode,bit_identical=True))
def invoke(cell,count,rounds):
    version,m,d,mode=cell
    return p.observations(p.run([binaries[version],bundle,m,d,count,rounds,mode,0,0,1]),m*d*4,count,rounds)
cells=[(v,m,d,mode) for m,d in shapes for mode in range(3) for v in binaries]
result=p.measure_paired(cells,invoke,lambda c:c[1]*c[2]*4,blocks=args.blocks,target_ms=30,adaptive=True,max_iterations=5000)
result.update(contract=__doc__,qualification=qualification,verification=verification,
              note='Positive means write-combined is faster than the bounded default-cache candidate, not the previous production baseline.')
p.evidence(args.output,result,[Path(__file__),metal,source/'native/metal/Metal.h',accept/'Driver.cpp',accept/'manifest.json',p.ROOT/'native/metal/UploadTests.cpp'],[*binaries.values(),build/'metal.a',accept/'task.a',*bundle.glob('*.metallib')],p.ROOT/'build/native/native/kalandra')
