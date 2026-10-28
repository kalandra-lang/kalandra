"""Frozen baseline versus prepared typed GPU execution, adjacent ABBA processes."""
import argparse,json,os,statistics,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline',type=Path,required=True)
parser.add_argument('--baseline-bundle',type=Path,required=True)
parser.add_argument('--current-evidence',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--blocks',type=int,default=5)
parser.add_argument('--allow-epilogue-extension',action='store_true',help='Require original shader prefix unchanged, allowing appended fused epilogues')
args=parser.parse_args();assert not args.output.exists()
for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES','KALANDRA_GPU_PROFILE'):assert os.getenv(flag) in (None,'','0'),flag
baseline=args.baseline.resolve();old=json.loads((baseline/'manifest.json').read_text())
for name,row in old.items():assert p.sha(baseline/name)==row['sha256'],name
current=json.loads(args.current_evidence.read_text())
headers=[p.ROOT/f for f in current['artifact_sha256'] if f.endswith('/TransformerProgram.h')];assert len(headers)==1
build=headers[0].parent;bundle=build/'bundle'
for f in [build/'gpu',*bundle.glob('*.metallib')]:assert p.sha(f)==current['artifact_sha256'][p.key(f)],f
# Require the same generated shader source, not merely a similar operation count.
shader_hashes={}
for f in [*bundle.glob('*.metal'),*bundle.glob('*.native.metal')]:
    previous=args.baseline_bundle/f.name;assert previous.exists(),f
    same=p.sha(f)==p.sha(previous)
    extension=args.allow_epilogue_extension and f.read_bytes().startswith(previous.read_bytes()) and b'inline float kalandra_fused_post' in f.read_bytes()
    assert same or extension,f
    shader_hashes[f.name]=dict(baseline=p.sha(previous),current=p.sha(f),relation='identical' if same else 'original prefix plus epilogue extension')
assert len(shader_hashes)>=7
binaries={'baseline':baseline/'gpu','current':build/'gpu'}
bundles={'baseline':args.baseline_bundle,'current':bundle}
shapes=[(1,64),(16,64),(64,64),(1,256),(16,256),(64,256),(1,512),(16,512),(64,512)]
cells=[(version,m,d,mode) for m,d in shapes for mode in range(3) for version in binaries]
verification=[]
for m,d in shapes:
    values=[]
    for version in binaries:
        raw=p.run([binaries[version],bundles[version],m,d,1,1,0,1,1]);rows=json.loads(raw)
        values.append(rows)
    assert values[0]==values[1],(m,d)
    verification.append(dict(m=m,d=d,pattern=1,bit_identical=True,cells=m*d))
def invoke(cell,count,rounds):
    version,m,d,mode=cell
    return p.observations(p.run([binaries[version],bundles[version],m,d,count,rounds,mode,0,0]),m*d*4,count,rounds)
data=p.measure_paired(cells,invoke,lambda cell:cell[1]*cell[2]*4,blocks=args.blocks,target_ms=20,adaptive=True,max_iterations=5000)
data.update(contract=__doc__,timing='Warm synchronous tensor-result API, full ACK and retirement; no output readback in timing. Pipeline loading and immutable plan preparation excluded in both cases; current records preparation_ns separately. Modes: 0 resident, 1 activation upload, 2 all seven uploads. Same iterations per pair, calibrated to the faster implementation.',verification=verification,shader_source_relationship=shader_hashes,baseline_manifest=old,current_evidence_sha256=p.sha(args.current_evidence))
sources=[Path(__file__),args.current_evidence,baseline/'manifest.json',baseline/'Driver.cpp',baseline/'Program.h',build/'TransformerProgram.h']
p.evidence(args.output,data,sources,[*binaries.values(),*bundle.glob('*.metallib'),*args.baseline_bundle.glob('*.metallib'),baseline/'metal.a',baseline/'task.a'],p.ROOT/'build/native/native/kalandra')
