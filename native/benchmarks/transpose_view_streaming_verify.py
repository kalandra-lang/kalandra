"""Check every streaming-policy output against the frozen qualified baseline."""
import json,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
out=p.ROOT/'benchmarks/results/gpu-transpose-view-streaming-verification-2026-10-29.json'
assert not out.exists()
base=p.ROOT/'benchmarks/.build/gpu-host-costs-2026-10-29/acceptance'
current=p.ROOT/'benchmarks/.build/gpu-transpose-view-2026-10-29/acceptance'
bundle=p.ROOT/'benchmarks/.build/gpu-attention-2026-10-29/acceptance/bundle'
evidence=json.loads((p.ROOT/'benchmarks/results/gpu-transpose-view-paired-2026-10-29.json').read_text())
for f,h in evidence['artifact_sha256'].items():assert p.sha(p.ROOT/f)==h,f
shapes=list(dict.fromkeys((r['m'],r['d'],r['pattern']) for r in evidence['verification']))
checks=[]
for m,d,pattern in shapes:
 expected=json.loads(p.run([base/'gpu',bundle,m,d,1,1,0,1,pattern,1,1,1]))
 for view in (0,1):
  for mode in range(3):
   actual=json.loads(p.run([current/'gpu',bundle,m,d,1,1,mode,1,pattern,1,1,1,view,0]))
   assert actual==expected,(m,d,pattern,view,mode)
   checks.append(dict(m=m,d=d,pattern=pattern,view=bool(view),mode=mode,bit_identical=True))
p.evidence(out,dict(contract=__doc__,checks=checks,numerical_bounds='Identical to the frozen output already checked against independent CPU/MPS bounds in the paired study.',paired_evidence_sha256=p.sha(p.ROOT/'benchmarks/results/gpu-transpose-view-paired-2026-10-29.json')),[Path(__file__)],[base/'gpu',current/'gpu',*bundle.glob('*.metallib')],current/'kalandra')
print(f'{len(checks)} streaming output comparisons passed')
