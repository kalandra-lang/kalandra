"""Audit full-model Steel acceptance without selecting away unfavorable results."""
import argparse,hashlib,json,statistics
from pathlib import Path
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--prefix',type=Path,required=True)
ap.add_argument('--build',type=Path,required=True)
ap.add_argument('--output',type=Path,required=True)
a=ap.parse_args();assert not a.output.exists()
load=lambda path:json.loads(path.read_text())
sha=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
qpath=a.build/'qualification.json';q=load(qpath)
assert len(q['checks'])==486 and all(x['passed'] for x in q['checks'])
assert len(q['oracles'])==36 and len(q['mode_checks'])==32 and len(q['references'])==12
inputs=[qpath,a.build/'manifest.json',Path(__file__)]
rows=[];loads=[];artifacts=None
for study in ('aa','frozen','policy','repeat','mps'):
 path=a.prefix.with_name(a.prefix.name+'-'+study+'.json')
 loadpath=path.with_name(path.stem+'-load.json')
 x=load(path);external=load(loadpath);inputs += [path,loadpath]
 assert x['study']==study and x['qualification_sha256']==sha(qpath)
 assert x['protocol']['blocks']==3 and x['protocol']['rounds']==3
 assert len(x['pairs'])==12 and len(x['observations'])==48
 assert x['artifact_sha256']==(artifacts or x['artifact_sha256']);artifacts=x['artifact_sha256']
 assert x['source_sha256']==q['source_sha256']
 assert x['caches'] and all(r['samples'] for r in x['caches'] if r['kind']!='mps')
 for group in ((1,512,0),(64,512,0),(512,512,0),(512,512,2)):
  pairs=[r for r in x['pairs'] if tuple(r['group'])==group];assert len(pairs)==3
  speedups=[r['speedup'] for r in pairs]
  medians={v:next(r['median_ns'] for r in x['summary'] if r['cell']==[v,*group]) for v in ('baseline','current')}
  setup={}
  for version in ('baseline','current'):
   samples=[s['pipeline_setup_ns'] for r in x['observations'] if r['cell']==[version,*group] for s in r['samples'] if s['round']==0 and 'pipeline_setup_ns' in s]
   if samples:setup[version]=statistics.median(samples)
  baseline=[r['baseline_ns'] for r in pairs]
  time_changes=[1/v-1 for v in speedups]
  control=study!='aa' or (abs(statistics.median(time_changes))<=.03 and max(map(abs,time_changes))<=.06)
  rows.append(dict(study=study,group=group,baseline_ns=medians['baseline'],current_ns=medians['current'],
    median_paired_speedup=statistics.median(speedups),min_paired_speedup=min(speedups),max_paired_speedup=max(speedups),
    baseline_block_range=max(baseline)/min(baseline),control_pass=control,pipeline_setup_ns=setup))
 samples=[r['external_cpu_percent'] for r in external['samples']];assert samples
 loads.append(dict(study=study,median_external_cpu_percent=statistics.median(samples),max_external_cpu_percent=max(samples),samples=len(samples)))
metrics=[r['metrics'] for r in q['oracles']]+[m for r in q['mode_checks'] for m in r['metrics']]+[r['metrics'] for r in q['references']]
data=dict(contract=__doc__,controls_pass=all(r['control_pass'] for r in rows),rows=rows,loads=loads,
 qualification=dict(full_oracle_outputs=len(metrics),full_oracle_cells=sum(r['cells'] for r in metrics),
  changing_input_comparisons=sum(r['comparisons'] for r in q['checks']),max_error_bound_ratio=max(r['max_error_bound_ratio'] for r in metrics)),
 input_sha256={str(p):sha(p) for p in inputs})
a.output.write_text(json.dumps(data,indent=2)+'\n')
print(json.dumps(data,indent=2))
