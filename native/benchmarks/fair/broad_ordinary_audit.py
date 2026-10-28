"""Audit broader CPU results using Fraction rounding, modular powers and divisors."""
import argparse,functools,hashlib,json,math,statistics,struct
from fractions import Fraction
from pathlib import Path
@functools.cache
def oracle(name,seed,n):
 if name.startswith('array'):
  modulus=1000003;scale=pow(13,n,modulus);offset=7*(scale-1)*pow(12,-1,modulus)%modulus
  return float(sum((scale*x+offset)%modulus for x in range(seed,seed+int(name[5:]))))
 if name in ('euclid','euclid_mixed'):
  b=seed*7+11;divisors=[d for d in range(1,b+1) if b%d==0];x=seed
  for _ in range(n):
   next=max(d for d in divisors if (x*13+7)%d==0)+seed
   x=(x*48271+next)%2147483647 if name=='euclid_mixed' else next
  return float(x)
 x=float(Fraction(seed,4 if name=='affine' else 64));scale=Fraction(1,2) if name=='affine' else Fraction(65,64);offset=Fraction(seed,16 if name=='affine' else 256)
 for _ in range(n):x=float(Fraction.from_float(float(Fraction.from_float(x)*scale))+offset)
 return x

def bits(x):return struct.unpack('<Q',struct.pack('<d',x))[0]
def audit(path,artifacts):
 d=json.loads(path.read_text());count=0
 for file,text in d['source_snapshots'].items():assert hashlib.sha256(text.encode()).hexdigest()==d['source_sha256'][file]
 if artifacts:
  for file,h in d['artifact_sha256'].items():assert hashlib.sha256(Path(file).read_bytes()).hexdigest()==h,file
 for row in d.get('verification',[]):
  _,name,n=row['cell'];expected=bits(oracle(name,row['seed'],n));assert [s['round'] for s in row['samples']]==[-2,-1,0]
  for sample in row['samples']:assert sample['checksum']==expected and math.isfinite(sample['ns']) and sample['ns']>=0;count+=1
 for row in d.get('calibration',[]):
  _,name,n=row['cell'];expected=(bits(oracle(name,17,n))*row['iterations'])&((1<<64)-1)
  assert row['iterations']>0 and [s['round'] for s in row['samples']]==list(range(-2,3))
  for sample in row['samples']:assert sample['checksum']==expected and math.isfinite(sample['ns']) and sample['ns']>=0;count+=1
 rounds=d['protocol']['rounds'];blocks=d['protocol'].get('blocks');medians={}
 for row in d['observations']:
  _,name,n=row['cell'];expected=(bits(oracle(name,17,n))*row['iterations'])&((1<<64)-1)
  assert row['iterations']>0 and [s['round'] for s in row['samples']]==list(range(-2,rounds))
  for sample in row['samples']:assert sample['checksum']==expected and math.isfinite(sample['ns']) and sample['ns']>0;count+=1
  medians.setdefault(tuple(row['cell']),[]).append(statistics.median(s['ns']/row['iterations'] for s in row['samples'] if s['round']>=0))
 for row in d['summary']:
  cell=tuple(row['cell']);assert row['median_ns']==statistics.median(medians[cell]) and len(medians[cell])==(blocks*2 if blocks else d['protocol']['processes'])
  assert bits(row['expected'])==bits(oracle(cell[1],17,cell[2]))
 for pair in d.get('pairs',[]):
  rows=sorted((r for r in d['observations'] if r['process']==pair['block'] and r['cell'][1:]==pair['group']),key=lambda r:r['slot'])
  order=['baseline','current','current','baseline'] if pair['block']%2==0 else ['current','baseline','baseline','current']
  assert len(rows)==4 and [r['cell'][0] for r in rows]==order and len({r['iterations'] for r in rows})==1
  before=[];after=[]
  for row in rows:
   m=statistics.median(s['ns']/row['iterations'] for s in row['samples'] if s['round']>=0)
   (before if row['cell'][0]=='baseline' else after).append(m)
  b,c=statistics.mean(before),statistics.mean(after)
  assert pair['baseline_ns']==b and pair['current_ns']==c and pair['speedup']==b/c and pair['time_reduction']==1-c/b
 return dict(path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),verified=True,artifact_files_verified=artifacts,verification_workloads=len(d.get('verification',[])),samples=count,measured_processes=len(d['observations']))
def main():
 p=argparse.ArgumentParser();p.add_argument('inputs',type=Path,nargs='+');p.add_argument('--output',type=Path,required=True);p.add_argument('--verify-artifacts',action='store_true');a=p.parse_args();assert not a.output.exists()
 a.output.write_text(json.dumps([audit(path,a.verify_artifacts) for path in a.inputs],indent=2)+'\n')
if __name__=='__main__':main()
