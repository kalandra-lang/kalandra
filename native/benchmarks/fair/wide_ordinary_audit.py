"""Independent Fraction/signed-integer and statistics audit for wide inputs."""
import argparse,functools,hashlib,json,math,statistics,struct
from fractions import Fraction
from pathlib import Path
MASK=(1<<64)-1
@functools.cache
def oracle(name,seed,n):
 def rem(a,b):return a-math.trunc(Fraction(a,b))*b
 if name.startswith('array'):
  values=list(range(seed,seed+int(name[5:])))
  for _ in range(n):values=[rem(x*13+7,1000003) for x in values]
  return float(sum(values))
 if name in ('euclid','euclid_mixed'):
  x=seed
  for _ in range(n):
   a,b=x*13+7,seed*7+11;mag=math.gcd(a,b)
   while b:a,b=b,rem(a,b)
   assert abs(a)==mag
   x=rem(x*48271+a+seed,2147483647) if name=='euclid_mixed' else a+seed
  return float(x)
 x=float(Fraction(seed,4 if name=='affine' else 64));offset=Fraction(seed,16 if name=='affine' else 256);scale=Fraction(1,2) if name=='affine' else Fraction(65,64)
 for _ in range(n):x=float(Fraction.from_float(float(Fraction.from_float(x)*scale))+offset)
 return x

def bits(x):return struct.unpack('<Q',struct.pack('<d',x))[0]
def audit(path,verify):
 d=json.loads(path.read_text());total=0;medians={}
 for name,text in d['source_snapshots'].items():assert hashlib.sha256(text.encode()).hexdigest()==d['source_sha256'][name]
 if verify:
  for f,h in d['artifact_sha256'].items():assert hashlib.sha256(Path(f).read_bytes()).hexdigest()==h,f
 expected={(v,n,r,c) for v in (('baseline','current') if 'pairs' in d else ('kalandra','cpp','rust')) for n in ('affine','polynomial','euclid','euclid_mixed','array32','array128') for r in ('wide-positive','wide-negative','rotating') for c in (1,16,4096)}
 assert {tuple(r['cell']) for r in d['summary']}==expected and len(d['summary'])==len(expected)
 def check(row,rounds):
  assert row['iterations']>0
  _,name,regime,n=row['cell'];values=[bits(oracle(name,s,n)) for s in d['seeds'][regime]];whole,tail=divmod(row['iterations'],8);expected=(sum(values)*whole+sum(values[:tail]))&MASK
  assert [r['round'] for r in row['samples']]==list(range(-2,rounds))
  for r in row['samples']:assert r['checksum']==expected and math.isfinite(r['ns']) and r['ns']>=0
  return len(row['samples'])
 for r in d.get('verification',[]):total+=check(r,1)
 for r in d['calibration']:total+=check(r,3)
 for r in d['observations']:
  total+=check(r,d['protocol']['rounds']);medians.setdefault(tuple(r['cell']),[]).append(statistics.median(s['ns']/r['iterations'] for s in r['samples'] if s['round']>=0))
 for r in d['summary']:
  assert len(medians[tuple(r['cell'])])==(2*d['protocol']['blocks'] if 'pairs' in d else d['protocol']['processes'])
  assert r['median_ns']==statistics.median(medians[tuple(r['cell'])]);_,name,regime,n=r['cell'];assert list(map(bits,r['expected']))==[bits(oracle(name,s,n)) for s in d['seeds'][regime]]
 for pair in d.get('pairs',[]):
  rows=sorted([r for r in d['observations'] if r['process']==pair['block'] and r['cell'][1:]==pair['group']],key=lambda r:r['slot']);order=['baseline','current','current','baseline'] if pair['block']%2==0 else ['current','baseline','baseline','current'];assert [r['cell'][0] for r in rows]==order and len({r['iterations'] for r in rows})==1
  v={s:[] for s in ('baseline','current')}
  for r in rows:v[r['cell'][0]].append(statistics.median(x['ns']/r['iterations'] for x in r['samples'] if x['round']>=0))
  b,c=map(lambda k:statistics.mean(v[k]),('baseline','current'));assert (pair['baseline_ns'],pair['current_ns'],pair['time_reduction'])==(b,c,1-c/b)
 return dict(path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),verified=True,artifacts_verified=verify,samples=total,processes=len(d['observations']))
def main():
 p=argparse.ArgumentParser();p.add_argument('inputs',type=Path,nargs='+');p.add_argument('--verify-artifacts',action='store_true');p.add_argument('--output',type=Path,required=True);a=p.parse_args();assert not a.output.exists();a.output.write_text(json.dumps([audit(f,a.verify_artifacts) for f in a.inputs],indent=2)+'\n')
if __name__=='__main__':main()
