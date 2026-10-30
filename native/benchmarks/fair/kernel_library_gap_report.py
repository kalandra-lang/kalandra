"""Audit current library gap, kernel intervals and rejected strict kernel screens."""
import gzip,json,statistics as st
from pathlib import Path
import protocol as p
R=p.ROOT/'benchmarks/results';B=p.ROOT/'benchmarks/.build/gpu-kernel-gap-2026-10-30'
def load(path):return json.loads(path.read_text())
def identity(x):
 for field in ('source_sha256','artifact_sha256','signature','artifacts'):
  for name,digest in x.get(field,{}).items():assert p.sha(p.ROOT/name)==digest,name
q=load(B/'qualification.json');identity(q);identity(load(B/'manifest.json'))
assert len(q['checks'])==24 and q['counter_bit_identical'] and all(x['metrics']['max_error_bound_ratio']<=1 for x in q['checks'])
lines=['# GPU kernel and numerical-library gap — 30 October 2026','','Production source is `8814a901`. No new production optimization is accepted in this investigation. The same authored float32 transformer block is compared through warm synchronous tensor-result APIs, with resident operands, completed GPU acknowledgement and cleanup, no timed readback. MPSGraph is a benchmark-only numerical-library reference; it need not use Kalandra’s exact product/reduction order or resource/cancellation contract. Both satisfy the independent propagated numerical bounds. This is not an equivalent scalar-source language benchmark.','','Twenty-four full-output checks pass across odd, small, 64-token and 512-token shapes, two input patterns and the production/MPSGraph/counter paths. The counter copy is bit-identical to production for every checked output.','','## Matched uninstrumented library comparison','','Three adjacent ABBA/BAAB process blocks, matching iterations per pair, two warmups, three measured rounds, 500 ms adaptive target and 8,192-call ceiling. Both graph/pipeline setup and initial resident uploads are excluded. AA uses the identical current Kalandra executable on both sides. Larger runtime/MPSGraph ratios mean a larger library gap. No samples are removed.','','| Study | Tokens × width | Kalandra / reference time | Block range | Control precision |','|---|---|---:|---:|---|']
for study in ('aa','library','repeat'):
 data=load(R/f'gpu-kernel-library-{study}-2026-10-30.json');identity(data);assert data['qualification_sha256']==p.sha(B/'qualification.json')
 for obs in data['observations']:p.observations('\n'.join(json.dumps(x) for x in obs['samples']),obs['cell'][1]*obs['cell'][2]*4,obs['iterations'],3)
 for pair in data['pairs']:
  values={'baseline':[],'current':[]}
  for obs in data['observations']:
   if obs['process']==pair['block'] and obs['cell'][1:]==pair['group']:values[obs['cell'][0]].append(st.median(x['ns']/obs['iterations'] for x in obs['samples'] if x['round']>=0))
  before=st.mean(values['baseline']);after=st.mean(values['current'])
  assert abs(pair['baseline_ns']-before)<.001 and abs(pair['current_ns']-after)<.001 and abs(pair['speedup']-before/after)<1e-12
 for group in sorted({tuple(x['group']) for x in data['pairs']}):
  pairs=[x for x in data['pairs'] if tuple(x['group'])==group];v=[x['speedup'] for x in pairs];drift=[100*x['time_reduction'] for x in pairs];passes=abs(st.median(drift))<=2 and max(map(abs,drift))<=5
  lines.append(f'| {study} | {group[0]} × {group[1]} | {st.median(v):.3f}× | {min(v):.3f}–{max(v):.3f}× | {("pass" if passes else "fail") if study=="aa" else "—"} |')
 assert load(R/f'gpu-kernel-library-{study}-2026-10-30-load.json')['samples']
profile=load(R/'gpu-kernel-library-profile-2026-10-30.json');identity(profile);assert not profile['acceptance']
labels={9:'Attention RMSNorm',11:'Q projection',13:'K projection',15:'V projection',16:'K transpose',18:'QKᵀ + scale',20:'Softmax',22:'Probabilities × V',24:'Output projection + residual',27:'FFN RMSNorm',29:'FFN expansion + ReLU',32:'FFN contraction + residual'}
lines+=['','## Separate encoder diagnosis','','Counter sampling changes execution overhead. Intervals include stalls/scheduling and their sum is not an exclusive wall-time decomposition. Three processes per shape execute 128 warmups and 192 measured calls. These timings are never used as library acceptance.','','| Tokens | Encoder | Median interval µs | Process range µs |','|---|---|---:|---:|']
for m in (64,512):
 records=[x for x in profile['records'] if x['m']==m];assert len(records)==3
 for row in records:
  raw=p.ROOT/row['raw'];assert p.sha(raw)==row['raw_sha256'];captured=load_gzip=None
  with gzip.open(raw,'rt') as stream:captured=json.load(stream)
  samples=[json.loads(s) for s in captured['stderr'].splitlines() if s.startswith('{"dispatch_counter_call":')];assert len(samples)==320
  for i,entry in enumerate(row['passes']):assert entry['median_ns']==st.median(x['passes'][i]['ns'] for x in samples[128:])
 for pc in sorted(labels,key=lambda pc:-st.median(next(v['median_ns'] for v in x['passes'] if v['pc']==pc) for x in records)):
  values=[next(v['median_ns'] for v in x['passes'] if v['pc']==pc)/1000 for x in records]
  lines.append(f'| {m} | {labels[pc]} | {st.median(values):.2f} | {min(values):.2f}–{max(values):.2f} |')
lines+=['','## Strict wider-tile experiments','','Both use the production-aligned 2D grid and exact balanced products/sums. Pair32 reuses A across two output columns; the second experiment halves K panels to reduce shared/private live resources while retaining all carry roots. Each passes 84 full-output comparisons including non-dyadic finite data, IEEE patterns, signed zero, non-power-of-two chunk counts, unchanged inputs and output guard bytes. These are isolated preallocated-kernel screens, not managed whole-model speedups. Both are rejected.','','| Tile | M × K × N | Median time reduction | Block range |','|---|---|---:|---:|']
for stem in ('gpu-wide-grid','gpu-wide-grid-leaf32'):
 q=load(R/f'{stem}-qualification-2026-10-30.json');identity(q);assert len(q['checks'])==84 and all(x['passed'] for x in q['checks'])
 data=load(R/f'{stem}-paired-2026-10-30.json');identity(data);assert data['qualification_sha256']==p.sha(R/f'{stem}-qualification-2026-10-30.json')
 for pair in data['pairs']:
  values={'baseline':[],'current':[]}
  for obs in data['observations']:
   if obs['process']==pair['block'] and obs['cell'][1:]==pair['group']:values[obs['cell'][0]].append(st.median(x['ns']/obs['iterations'] for x in obs['samples'] if x['round']>=0))
  assert abs(pair['time_reduction']-(1-st.mean(values['current'])/st.mean(values['baseline'])))<1e-12
 for group in sorted({tuple(x['group']) for x in data['pairs']}):
  v=[x['time_reduction']*100 for x in data['pairs'] if tuple(x['group'])==group];lines.append(f'| {stem} | {group[0]} × {group[1]} × {group[2]} | {st.median(v):+.2f}% | {min(v):+.2f}% … {max(v):+.2f}% |')
lines+=['','The first screen has no external-load record: sampler PID discovery failed and the verified PID had exited by the second attachment. That limitation is explicitly retained. The second screen records load from process launch; its context string inherited an old cache-study label, while its actual command/source/artifact identities identify this isolated kernel screen. Neither is promoted or used as proof of an uncontended production regression.','','The matched 512-token library gap and dominant FFN intervals point to matrix computation, not more invocation bookkeeping. Failed layouts do not prove every strict kernel strategy is exhausted, or that register pressure alone caused slowdowns. No rounding relaxation or production vendor dependency is introduced.']
(R/'gpu-kernel-library-gap-2026-10-30.md').write_text('\n'.join(lines)+'\n');print('Audited matched library comparison, encoder intervals and both rejected strict screens.')
