"""Independent signed-zero-preserving verification of the diagnostic host profile."""
import json,os,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import transformer_reference as ref
profile=p.ROOT/'benchmarks/results/gpu-reuse-host-profile-2026-10-29.json'
saved=json.loads(profile.read_text())
for field in ('source_sha256','artifact_sha256'):
 for name,digest in saved[field].items():assert p.sha(p.ROOT/name)==digest,name
b=p.ROOT/'benchmarks/.build/gpu-reuse-host-profile-2026-10-29';f=p.ROOT/'benchmarks/.build/gpu-upload-cache-atomic-2026-10-29'
checks=[];oracles=[]
for m,d in ((1,512),(16,512),(64,512),(64,256)):
 for pattern in (0,1):
  for mode in (0,1,2):
   rows=[]
   for exe in (f/'candidate',b/'profile'):
    rows.append(json.loads(p.run([exe,f/'bundle',m,d,1,1,mode,1,pattern,2,1],env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')),parse_int=lambda n:-0.0 if n=='-0' else int(n)))
   bits=lambda row:struct.pack('<'+'f'*len(row['cells']),*row['cells'])
   assert bits(rows[0])==bits(rows[1]);checks.append(dict(m=m,d=d,pattern=pattern,mode=mode,passed=True))
   if mode==0:
    metrics,_=ref.assess(m,d,pattern,rows[1]);oracles.append(dict(m=m,d=d,pattern=pattern,metrics=metrics))
p.evidence(p.ROOT/'benchmarks/results/gpu-reuse-host-verification-2026-10-29.json',dict(checks=checks,oracles=oracles,profile_sha256=p.sha(profile)),[Path(__file__),profile,Path(ref.__file__)],[f/'candidate',b/'profile'],p.ROOT/'build/native/native/kalandra')
print('24 signed-zero-preserving full outputs and eight independent oracles passed.')
