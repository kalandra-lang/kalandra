"""Offline integrity and paired-calculation checks for memory-access diagnostics."""
import hashlib,json,math,statistics
from pathlib import Path
root=Path(__file__).resolve().parents[2]
records=[]
for path in sorted((root/'benchmarks/results').glob('memory-access-*.json')):
    data=json.loads(path.read_text())
    if 'source_sha256' not in data or 'records' not in data:continue
    for field in ['driver_source','generator_source','runner_source','layout_source','generated_msl']:
        if field in data:assert hashlib.sha256(data[field].encode()).hexdigest() in data['source_sha256'].values(),(path,field)
    for r in data['records']+data.get('calibration',[]):
        assert [s['round'] for s in r['samples']]==list(range(-2,3)),path
        for s in r['samples']:
            assert s['cells']==r['shape'][0]*r['shape'][2]
            assert all(math.isfinite(s[k]) and s[k]>0 for k in ['ns','gpu_ns'])
            if r.get('sampled'):assert s['matrix_ticks']>0 and s['pack_ticks']>=0
    for pair in data.get('pairs',[]):
        group=[r for r in data['records'] if (r['block'],r['variant'],r['shape'])==(pair['block'],pair['variant'],pair['shape'])]
        assert len(group)==4 and [r['slot'] for r in group]==list(range(4))
        versions=['baseline','current','current','baseline'] if pair['block']%2==0 else ['current','baseline','baseline','current']
        assert [r['version'] for r in group]==versions
        med={v:statistics.mean(statistics.median(s['gpu_ns']/r['iterations'] for s in r['samples'] if s['round']>=0) for r in group if r['version']==v) for v in ['baseline','current']}
        assert math.isclose(pair['baseline_gpu_ns'],med['baseline'],abs_tol=1e-9)
        assert math.isclose(pair['current_gpu_ns'],med['current'],abs_tol=1e-9)
        assert math.isclose(pair['time_reduction'],1-med['current']/med['baseline'],abs_tol=1e-12)
    records.append(dict(path=str(path.relative_to(root)),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),processes=len(data['records']),calibration_processes=len(data.get('calibration',[])),pairs=len(data.get('pairs',[]))))
assert records
print(json.dumps(dict(status='passed',records=records,processes=sum(r['processes'] for r in records),calibration_processes=sum(r['calibration_processes'] for r in records),pairs=sum(r['pairs'] for r in records)),indent=2))
