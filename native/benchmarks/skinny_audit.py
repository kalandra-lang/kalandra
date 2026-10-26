"""Offline source, complete-cell count, checksum and paired-ratio audit."""
import hashlib
import json
import math
from pathlib import Path
import statistics
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import pipeline_reference as ref

root=Path(__file__).resolve().parents[2];records=[]
for path in sorted((root/'benchmarks/results').glob('skinny-*-2026-10-26.json')):
    if path.name.startswith('skinny-evidence-audit-'):
        continue
    data=json.loads(path.read_text())
    if 'source_metadata_correction' in data:
        correction=data['source_metadata_correction']
        original=json.loads(json.dumps(data));original.pop('source_metadata_correction')
        original['driver_source']=correction['original_reported_driver_source']
        original['source_sha256']['native/benchmarks/SkinnyMatmulStudy.mm']=correction['original_reported_driver_sha256']
        assert hashlib.sha256((json.dumps(original,indent=2)+'\n').encode()).hexdigest()==correction['original_record_sha256'],path
    if 'records' in data:
        for field in ('driver_source','generator_source','generated_msl','layout_source'):
            if field in data:
                assert hashlib.sha256(data[field].encode()).hexdigest() in data['source_sha256'].values(),(path,field)
        for row in data['records']+data.get('calibration',[]):
            assert [s['round'] for s in row['samples']]==list(range(-2,3)),path
            for sample in row['samples']:
                assert sample['cells']==row['shape'][0]*row['shape'][2]*2,path
                assert all(math.isfinite(sample[k]) and sample[k]>0 for k in ('ns','gpu_ns')),path
        for pair in data.get('pairs',[]):
            rows=[r for r in data['records'] if (r['block'],r['variant'],r['shape'])==(pair['block'],pair['variant'],pair['shape'])]
            assert len(rows)==4 and [r['slot'] for r in rows]==[0,1,2,3],path
            order=['baseline','current','current','baseline'] if pair['block']%2==0 else ['current','baseline','baseline','current']
            assert [r['version'] for r in rows]==order and len({r['iterations'] for r in rows})==1,path
            med={v:statistics.mean(statistics.median(s['gpu_ns']/(r['iterations']*2) for s in r['samples'] if s['round']>=0) for r in rows if r['version']==v) for v in ('baseline','current')}
            for v in med:assert math.isclose(pair[v+'_gpu_ns'],med[v],abs_tol=1e-9),path
            assert math.isclose(pair['time_reduction'],1-med['current']/med['baseline'],abs_tol=1e-12),path
        records.append(dict(path=str(path.relative_to(root)),sha256=p.sha(path),processes=len(data['records']),calibration_processes=len(data.get('calibration',[])),pairs=len(data.get('pairs',[]))))
    elif 'paired' in data or 'profiles' in data:
        rows=data.get('paired',{}).get('observations',data.get('profiles',[]))
        for row in rows:
            value=ref.exact(row['cell'][-1]) if len(row['cell'])==3 else ref.exact_rect(*row['cell'][-3:])
            expected=value*row['iterations']
            assert [s['round'] for s in row['samples']]==list(range(-2,3)),path
            for sample in row['samples']:
                assert sample['checksum']==expected and math.isfinite(sample['ns']) and sample['ns']>0,path
                assert ('gpu_ns' in sample)==('profiles' in data),path
        for pair in data.get('paired',{}).get('pairs',[]):
            group=[r for r in rows if r['process']==pair['block'] and r['cell'][1:]==pair['group']]
            assert len(group)==4 and [r['slot'] for r in group]==[0,1,2,3],path
            order=['baseline','current','current','baseline'] if pair['block']%2==0 else ['current','baseline','baseline','current']
            assert [r['cell'][0] for r in group]==order and len({r['iterations'] for r in group})==1,path
            med={v:statistics.mean(statistics.median(s['ns']/r['iterations'] for s in r['samples'] if s['round']>=0) for r in group if r['cell'][0]==v) for v in ('baseline','current')}
            for v in med:assert math.isclose(pair[v+'_ns'],med[v],abs_tol=1e-9),path
            assert math.isclose(pair['time_reduction'],1-med['current']/med['baseline'],abs_tol=1e-12),path
        records.append(dict(path=str(path.relative_to(root)),sha256=p.sha(path),processes=len(rows),pairs=len(data.get('paired',{}).get('pairs',[]))))
assert records
print(json.dumps(dict(status='passed',records=records,processes=sum(r['processes'] for r in records),pairs=sum(r['pairs'] for r in records),calibration_processes=sum(r.get('calibration_processes',0) for r in records)),indent=2))
