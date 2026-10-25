"""Verify packing evidence snapshots, round coverage and paired calculations offline."""
import hashlib,json,math,statistics
from pathlib import Path
root=Path(__file__).resolve().parents[2];results=root/'benchmarks/results'
sha=lambda b:hashlib.sha256(b).hexdigest()
rows=[]
for path in sorted(results.glob('weight-packing-*.json')):
 if any(kind in path.name for kind in ('qualification','shader-validation')):continue
 d=json.loads(path.read_text());assert len(d['records'])>0
 for field,name in [('driver_source','WeightPackingStudy.mm'),('generator_source','weight_packing.py'),('runner_source','weight_packing_boundaries.py')]:
  if field in d:
   matches=[h for k,h in d['source_sha256'].items() if k.endswith('/'+name)]
   assert matches==[sha(d[field].encode())],(path.name,field,matches)
 if 'generated_msl' in d:
  assert sha(d['generated_msl'].encode()) in d['source_sha256'].values()
 for rec in d['records']+d.get('calibration',[]):
  samples=rec['samples'];assert [s['round'] for s in samples]==list(range(-2,3))
  square=rec.get('n',rec['cell'][1] if 'cell' in rec else None)
  shape=rec.get('shape',[square]*3);reuse=rec.get('reuse',rec['cell'][2] if 'cell' in rec else 1)
  cells=shape[0]*shape[2]*reuse
  for sample in samples:
   assert sample['cells']==cells
   assert all(math.isfinite(sample[k]) and sample[k]>0 for k in ['ns','gpu_ns','private_bytes','shared_bytes'])
   assert sample['workspace_bytes']>=0
 for pair in d.get('pairs',[]):
  before=pair.get('baseline_gpu_ns',pair.get('baseline_gpu_ns_per_call'))
  after=pair.get('current_gpu_ns',pair.get('current_gpu_ns_per_call'))
  assert before>0 and after>0 and math.isclose(pair['time_reduction'],1-after/before,abs_tol=1e-12)
 for pair in d.get('pairs',[]):
  group=[r for r in d['records'] if r['block']==pair['block'] and (r.get('shape')==pair['shape'] if 'shape'in pair else (r['layout'],r['n'],r['reuse'])==(pair['layout'],pair['n'],pair['reuse']))]
  assert len(group)==4 and [r['slot'] for r in group]==list(range(4))
  assert [r['version'] for r in group] == (['baseline','current','current','baseline'] if pair['block']%2==0 else ['current','baseline','baseline','current'])
  med={v:statistics.mean(statistics.median(s['gpu_ns']/r['iterations']/r.get('reuse',1) for s in r['samples'] if s['round']>=0) for r in group if r['version']==v) for v in ['baseline','current']}
  assert math.isclose(1-med['current']/med['baseline'],pair['time_reduction'],abs_tol=1e-12)
 rows.append(dict(file=path.name,sha256=sha(path.read_bytes()),measurement_processes=len(d['records']),calibration_processes=len(d.get('calibration',[])),paired_blocks=len(d.get('pairs',[]))))
assert rows, "no packing records found"
print(json.dumps(dict(records=rows,measurement_processes=sum(r['measurement_processes'] for r in rows),calibration_processes=sum(r['calibration_processes'] for r in rows),paired_blocks=sum(r['paired_blocks'] for r in rows),status='passed'),indent=2))
