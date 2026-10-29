"""Isolated control: retain staged binary maps for single-row matrix outputs."""
import argparse,io,json,shutil,struct,subprocess,sys,tarfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'));import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
for arg in ('evidence','build-dir','output'):parser.add_argument('--'+arg,type=Path,required=True)
args=parser.parse_args();assert not args.output.exists();build=args.build_dir.resolve();build.mkdir(parents=True,exist_ok=False)
data=json.loads(args.evidence.read_text());accept=next(p.ROOT/f for f in data['artifact_sha256'] if f.endswith('/TransformerProgram.h')).parent;bundle=accept/'bundle';base=p.ROOT/'benchmarks/.build/gpu-attention-2026-10-29/baseline'
source=build/'source';source.mkdir()
with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(['git','archive','2fb73f53','native'],cwd=p.ROOT))) as archive:archive.extractall(source,filter='data')
metal=source/'native/metal/Metal.mm';text=(p.ROOT/'native/metal/Metal.mm').read_text();needle='    // Peek only at a guard that already passes from output-shape metadata.';assert text.count(needle)==1
metal.write_text(text.replace(needle,'    if (post.inputs.size() > 1 && a.shape()[0] == 1) return false;\n'+needle))
shutil.copy2(p.ROOT/'native/metal/MatmulLayout.h',source/'native/metal/MatmulLayout.h')
flags=['-std=c++23','-O2','-pthread','-mmacosx-version-min=26.0']
p.run([p.CLANG,*flags,'-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs','-c',metal,'-o',build/'Metal.o']);p.run(['ar','rcs',build/'metal.a',build/'Metal.o'])
cpp=build/'Driver.cpp';cpp.write_text((base/'Driver.cpp').read_text().replace('../../metal/Metal.h',str(source/'native/metal/Metal.h')))
p.run([p.CLANG,*flags,'-ffp-contract=off','-I'+str(accept),'-I'+str(p.ROOT/'native/benchmarks/fair'),cpp,build/'metal.a',base/'task.a','-framework','Foundation','-framework','Metal','-o',build/'gpu'])
binaries={'baseline':accept/'gpu','current':build/'gpu'};shapes=[(1,64),(1,256),(1,512),(64,512)];verification=[]
for m,d in shapes:
 values=[json.loads(p.run([b,bundle,m,d,1,1,0,1,1])) for b in binaries.values()];assert values[0]==values[1]
 verification.append(dict(m=m,d=d,bit_identical=True))
def invoke(cell,count,rounds):
 v,m,d,mode=cell
 return p.observations(p.run([binaries[v],bundle,m,d,count,rounds,mode,0,0]),m*d*4,count,rounds)
cells=[(v,m,d,mode) for m,d in shapes for mode in range(3) for v in binaries]
result=p.measure_paired(cells,invoke,lambda c:c[1]*c[2]*4,blocks=3,target_ms=30,adaptive=True,max_iterations=5000)
result.update(contract=__doc__,verification=verification,acceptance_sha256=p.sha(args.evidence),note='Positive means staging single-row residuals is faster; large case is an unchanged-selection control.')
p.evidence(args.output,result,[Path(__file__),metal,cpp,source/'native/metal/MatmulLayout.h',args.evidence],[*binaries.values(),build/'metal.a',base/'task.a',*bundle.glob('*.metallib')],p.ROOT/'build/native/native/kalandra')
