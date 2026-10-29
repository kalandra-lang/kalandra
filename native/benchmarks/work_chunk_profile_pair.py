"""Paired, adequately batched diagnostic timing of the frozen profiled driver."""
import argparse,json,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--profile-evidence',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();assert not args.output.exists()
data=json.loads(args.profile_evidence.read_text())
for f,digest in data['artifact_sha256'].items():assert p.sha(p.ROOT/f)==digest
binary=next(p.ROOT/f for f in data['artifact_sha256'] if f.endswith('/profile'))
bundle=next(p.ROOT/f for f in data['artifact_sha256'] if f.endswith('.metallib')).parent
cells=[(version,m,512) for m in (1,64) for version in ('baseline','current')]
def invoke(cell,count,rounds):
    version,m,d=cell;current=version=='current'
    rows=p.observations(p.run([binary,bundle,m,d,count,rounds,0,0,0,1,int(current)]),m*d*4,count,rounds)
    commands=(1 if m==1 else 5) if current else (4 if m==1 else 6)
    assert all(r['dispatches']==12*count and r['submissions']==commands*count for r in rows)
    return rows
result=p.measure_paired(cells,invoke,lambda c:c[1]*c[2]*4,blocks=5,target_ms=30,adaptive=True,max_iterations=5000)
result.update(contract=__doc__,profile_evidence_sha256=p.sha(args.profile_evidence),note='Diagnostic only, separate from acceptance. GPU command intervals include dispatch overhead and gaps within commands; changing command boundaries also changes that accounting. Recorded encoding is not an exhaustive host-time profile.')
p.evidence(args.output,result,[Path(__file__),args.profile_evidence],[p.ROOT/f for f in data['artifact_sha256']],p.ROOT/'build/native/native/kalandra')
