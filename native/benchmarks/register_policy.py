"""Screen matrix-kernel selection with an isolated runtime; numerical kernels are unchanged."""
import argparse,io,json,shutil,struct,subprocess,sys,tarfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'));import protocol as p
BASE='f28a71f0d0d4bc11c806b638e8404b46b4451456'
parser=argparse.ArgumentParser(description=__doc__)
for flag in ('baseline','bundle','build-dir','output'):parser.add_argument('--'+flag,type=Path,required=True)
parser.add_argument('--policy',choices=['long','wide'],required=True);parser.add_argument('--blocks',type=int,default=3)
args=parser.parse_args();assert not args.output.exists()
baseline=args.baseline.resolve();bundle=args.bundle.resolve();build=args.build_dir.resolve();build.mkdir(parents=True,exist_ok=False)
manifest=json.loads((baseline/'manifest.json').read_text())
for name,row in manifest.items():assert p.sha(baseline/name)==row['sha256']
source=build/'source';source.mkdir()
with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(['git','archive',BASE,'native'],cwd=p.ROOT))) as archive:archive.extractall(source,filter='data')
header=source/'native/metal/MatmulLayout.h';text=header.read_text();old='std::uint64_t(rows) * columns >= 256u * 256u;';assert text.count(old)==1
rule={'long':'(std::uint64_t(rows) * columns >= 65536u || (inner >= 1024 && std::uint64_t(rows) * columns >= 8192u));','wide':'std::uint64_t(rows) * columns >= 8192u;'}[args.policy]
header.write_text(text.replace(old,rule));metal=source/'native/metal/Metal.mm'
flags=['-std=c++23','-O2','-pthread','-mmacosx-version-min=26.0']
p.run([p.CLANG,*flags,'-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs','-c',metal,'-o',build/'Metal.o']);p.run(['ar','rcs',build/'metal.a',build/'Metal.o'])
cpp=build/'Driver.cpp';cpp.write_text((baseline/'Driver.cpp').read_text().replace('../../metal/Metal.h',str(source/'native/metal/Metal.h')))
shutil.copy2(baseline/'Program.h',build/'TransformerProgram.h')
p.run([p.CLANG,*flags,'-ffp-contract=off','-I'+str(build),'-I'+str(source/'native/benchmarks/fair'),cpp,build/'metal.a',baseline/'task.a','-framework','Foundation','-framework','Metal','-o',build/'gpu'])
binaries={'baseline':baseline/'gpu','current':build/'gpu'};shapes=[(m,d) for d in (64,256,512) for m in (1,16,64)];verification=[]
for m,d,pattern in [(2,3,1),(3,7,1),(5,17,1),(17,33,1)]+[(m,d,1) for m,d in shapes]:
    rows=[json.loads(p.run([binary,bundle,m,d,1,1,0,1,pattern])) for binary in binaries.values()]
    assert struct.pack('<'+'f'*len(rows[0]['cells']),*rows[0]['cells'])==struct.pack('<'+'f'*len(rows[1]['cells']),*rows[1]['cells'])
    verification.append(dict(m=m,d=d,cells=m*d,bit_identical=True))
def invoke(cell,count,rounds):
    version,m,d,mode=cell;return p.observations(p.run([binaries[version],bundle,m,d,count,rounds,mode,0,0]),m*d*4,count,rounds)
cells=[(version,m,d,mode) for m,d in shapes for mode in range(3) for version in binaries]
result=p.measure_paired(cells,invoke,lambda c:c[1]*c[2]*4,blocks=args.blocks,target_ms=20,adaptive=True,max_iterations=5000)
result.update(contract=__doc__,base_commit=BASE,policy=args.policy,rule=rule,verification=verification,baseline_manifest=manifest)
p.evidence(args.output,result,[Path(__file__),header,metal,cpp,baseline/'manifest.json'],[*binaries.values(),build/'metal.a',baseline/'metal.a',baseline/'task.a',*bundle.glob('*.metallib')],baseline/'compiler')
