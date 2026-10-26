"""Recompute CPU workload and recursion checks, statistics and artifact digests."""
from pathlib import Path
import argparse,json,hashlib,statistics,math,sys
root=Path.cwd();sys.path.insert(0,str(root/'native/benchmarks/fair'))
from runtime_workloads import oracle
sys.path.insert(0,str(root/'native/experiments/cancellation'))
import depth_run as depth
parser=argparse.ArgumentParser(description=__doc__)
for name in ('workloads','recursion','profile','output'): parser.add_argument('--'+name,type=Path,required=True)
parser.add_argument('--verify-artifacts',action='store_true')
args=parser.parse_args(); reports=[]
assert not args.output.exists()
hashfile=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
p=json.loads((args.workloads).read_text())
for name,source in p['source_snapshots'].items():assert hashlib.sha256(source.encode()).hexdigest()==p['source_sha256'][name]
if args.verify_artifacts:
 for name,h in p['artifact_sha256'].items(): assert hashfile(name)==h,name
for record in p['verification']:
 expected=oracle(record['cell'][1],record['seed'],record['cell'][2])
 assert [s['round'] for s in record['samples']]==[-2,-1,0]
 assert all(s['checksum']==expected and math.isfinite(s['ns']) for s in record['samples'])
processes=0
for result in (p,p['task_scope_diagnostic']):
 processes+=len(result['observations'])
 for record in result['observations']:
  count=record['iterations']; expected=oracle(record['cell'][1],17,record['cell'][2])
  assert count>0 and [r['round'] for r in record['samples']]==list(range(-2,7))
  assert all(r['checksum']==count*expected and math.isfinite(r['ns']) and r['ns']>0 for r in record['samples'])
 for row in result['summary']:
  records=[r for r in result['observations'] if r['cell']==row['cell']]
  values=[statistics.median(s['ns']/r['iterations'] for s in r['samples'] if s['round']>=0) for r in records]
  assert len(records)==3 and row['median_ns']==statistics.median(values)
reports.append(dict(path=str(args.workloads),sha256=hashfile(args.workloads),verification_workloads=len(p['verification']),measured_processes=processes,artifact_files_verified=args.verify_artifacts,verified=True))
p=json.loads((args.recursion).read_text())
if args.verify_artifacts:
 for name,h in p['hashes'].items():assert hashfile(name)==h,name
checks=[(v,k,d,s) for v in ('0','1','3','cpp','rust') for k in range(4) for d in ((0,1,100,1000,10000) if k==3 else (0,1,5,10,14)) for s in (1,17,31)]
assert len(checks)==len(p['verification'])==300
for row,(version,k,d,seed) in zip(p['verification'],checks):
 assert row['version']==version
 assert row['kind']==k and row['depth']==d and row['count']==1
 expected=float(depth.exact(k,d,seed))
 assert math.isfinite(row['checksum']) and abs(row['checksum']-expected)<=3*math.ulp(expected)
for r in p['samples']:
 count=r['count']; expected=float(depth.exact(r['kind'],r['depth'],17));eps=2**-53
 bound=count*2*math.ulp(expected)+((count-1)*eps/(1-(count-1)*eps))*count*abs(expected)+math.ulp(expected*count)
 assert math.isfinite(r['checksum']) and abs(r['checksum']-count*expected)<=bound
for row in p['summary']:
 kind=depth.NAMES.index(row['kind'])
 med={v:statistics.median(r['ns']/r['count'] for r in p['samples'] if r['version']==v and r['kind']==kind and r['depth']==row['depth']) for v in ('0','1','3','cpp','rust')}
 assert med==row['median_ns']
 assert row['hybrid_over_cpp']==med['3']/med['cpp'] and row['hybrid_over_rust']==med['3']/med['rust']
reports.append(dict(path=str(args.recursion),sha256=hashfile(args.recursion),verification_workloads=300,timed_samples=len(p['samples']),artifact_files_verified=args.verify_artifacts,verified=True))
p=json.loads((args.profile).read_text())
for r in p['records']:
 assert hashfile(r['sample_path'])==r['sample_sha256']
 if args.verify_artifacts: assert hashfile(r['command'][0])==r['binary_sha256']
reports.append(dict(path=str(args.profile),sha256=hashfile(args.profile),profiles=len(p['records']),verified=True))
(args.output).write_text(json.dumps(reports,indent=2)+'\n')
print(json.dumps(reports,indent=2))
