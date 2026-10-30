"""Audit production prepared-output cache correctness, caps and every paired sample."""
import json,statistics as st
from pathlib import Path
import protocol as p
R=p.ROOT/'benchmarks/results';B=p.ROOT/'benchmarks/.build/gpu-prepared-output-priority-cache-2026-10-29'
def load(name):return json.loads((R/name).read_text())
def identity(data):
 for field in ('source_sha256','artifact_sha256','signature','artifacts','sources'):
  for name,digest in data.get(field,{}).items():
   assert any(x.is_file() and p.sha(x)==digest for x in [p.ROOT/name,B/'frozen-sources'/name]),name
q=load('gpu-prepared-output-priority-qualification-2026-10-29.json');identity(q)
assert p.sha(B/'qualification.json')==p.sha(R/'gpu-prepared-output-priority-qualification-2026-10-29.json')
assert len(q['checks'])==486 and sum(x['comparisons'] for x in q['checks'])==1944 and all(x['passed'] for x in q['checks'])
assert {x['cap'] for x in q['checks']}=={0,4096,16*1024*1024}
assert len(q['oracles'])==18 and all(x['metrics']['max_error_bound_ratio']<=1 for x in q['oracles'])
for name in ('gpu-prepared-output-priority-manifest-2026-10-29.json','gpu-prepared-output-priority-frozen-manifest-2026-10-29.json'):identity(load(name))
tests=load('gpu-prepared-output-priority-test-record-2026-10-29.json');identity(tests)
assert tests['tests']==201 and not tests['failed']
assert tests['log_sha256']==p.sha(R/'gpu-prepared-output-priority-tests-2026-10-29.txt')
lines=['# Upload-protecting cache candidate — 29–30 October 2026','','Production runtime extends the accepted per-runtime upload cache with completed, fully overwritten Float32 prepared outputs. One combined 16 MiB / 64-entry idle cap remains inside the 64 MiB default payload budget. Last-owner retirement, zero admissions, fresh metadata and distinct shared/private keys preserve immutable ownership. No numerical, cancellation, checkpoint or shader change. Native C++ clients must rebuild for the extended Options/Stats layout.','','## Correctness and scope','','201 GPU runtime/support tests pass, including prepared-program integration in debug, optimized, ASan/UBSan and TSan modes. The dedicated fixture checks aliases, opposite prior bits (including NaNs/negative zero), changing ranks, storage-kind separation, exact budget pressure, all allocation-failure points, held acknowledgement, cancellation/deadlines, device failure, concurrency, policy changes, entry caps and teardown. This is not a full compiler/CLI suite rerun.','','1,944 full-output comparisons pass against frozen accepted production across nine shapes, all three runner/upload modes, both switch orders and zero / 4 KiB / 16 MiB cache limits. Eighteen independent high-precision oracle checks pass. Signed zero survives decoding. Numerical comparisons precede timing and never overlap it.','','## Same-runtime paired intervals','','A single runtime, queue, prepared program and resident inputs span ABBA/BAAB intervals. Each slot trims idle backing and warms two complete intervals outside timing; all per-call execution remains timed. Five blocks, 1,024 calls per measured slot, two initial warm-up slots. GPU/encoding counters are enabled equally on both sides. AA disables private caching on both sides; toggle and repeat switch it on only for the current side. The original precision requirement is absolute AA wall median ≤2% and every block within ±5%; a failure limits the corresponding gain claim.','','| Scope / study | Rows × width / upload mode | Wall reduction | Block range | GPU interval reduction | Encoding reduction | AA precision |','|---|---|---:|---:|---:|---:|---|']
maxidle=maxretained=0;results={};classlines=['','## Cache class behavior','','Counters below are per measured call. Idle byte counts are post-interval snapshots, not peak live working sets. Both classes share the same cap.','','| Scope / study | Rows / mode | Private enabled | Shared hits / misses | Private hits / misses | Shared / private idle bytes |','|---|---|---|---:|---:|---:|']
for scope in ('largest','guards'):
 for study in ('aa','toggle','repeat'):
  name=f'gpu-prepared-output-priority-{scope}-{study}-2026-10-29.json';data=load(name);identity(data)
  assert data['qualification_sha256']==p.sha(B/'qualification.json')
  assert data['protocol']==dict(blocks=5,iterations=1024,per_slot_warmup_intervals=2,initial_warmups=2,order='ABBA/BAAB',profile=True)
  assert len(data['observations'])==(1 if scope=='largest' else 3)
  for obs in data['observations']:
   group=obs['group'];m,d,mode=group;rows=obs['samples'];count=obs['iterations'];assert count==1024
   timed=[x for x in rows if 'ns' in x];cache=[x for x in rows if x.get('cache')];states=[x for x in rows if x.get('pair_state')];kinds=[x for x in rows if x.get('cache_kinds')]
   p.observations('\n'.join(json.dumps(x) for x in timed),m*d*4,count,20)
   assert len(timed)==len(cache)==len(states)==len(kinds)==22
   for field in ('attempts','dispatches','submissions'):assert len({x[field]/count for x in timed})==1
   for sample,c,state,k in zip(timed,cache,states,kinds):
    assert sample['round']==c['round']==state['round']==k['round']
    assert sample['gpu_ns']>0 and sample['encoding_ns']>0
    assert c['retained_bytes']==c['live_bytes']+c['cached_bytes'] and c['cached_bytes']==k['shared_bytes']+k['private_bytes']
    assert c['cached_bytes']<=16*1024*1024 and c['peak_retained_bytes']<=64*1024*1024
    if not state['enabled']:assert not k['private_bytes'] and not k['private_hits']
    if mode==0:assert bool(k['private_bytes'])==state['enabled']
    if mode==2:assert k['shared_hits']==count*7 and not k['shared_misses']
    assert state['enabled']==(state['current'] and study!='aa')
    maxidle=max(maxidle,c['cached_bytes']);maxretained=max(maxretained,c['peak_retained_bytes'])
   for enabled in (False,True):
    selected=[k for k,state in zip(kinds,states) if k['round']>=0 and state['enabled']==enabled]
    if selected:
     med=lambda key:st.median(x[key] for x in selected)
     classlines.append(f'| {scope} / {study} | {m} / {mode} | {enabled} | {med("shared_hits")/count:.2f} / {med("shared_misses")/count:.2f} | {med("private_hits")/count:.2f} / {med("private_misses")/count:.2f} | {med("shared_bytes"):,.0f} / {med("private_bytes"):,.0f} |')
   pairs=[x for x in data['pairs'] if x['group']==group];assert len(pairs)==5
   for pair in pairs:
    selected=[(r,s) for r,s in zip(timed,states) if r['round']>=0 and r['round']//4==pair['block']]
    before=[r for r,s in selected if not s['current']];after=[r for r,s in selected if s['current']];assert len(before)==len(after)==2
    for field in ('ns','gpu_ns','encoding_ns'):
     old=st.mean(x[field]/count for x in before);new=st.mean(x[field]/count for x in after);metric=pair['metrics'][field]
     assert abs(metric['baseline']-old)<.001 and abs(metric['current']-new)<.001 and abs(metric['reduction']-(1-new/old))<1e-12
   metrics={field:[x['metrics'][field]['reduction']*100 for x in pairs] for field in ('ns','gpu_ns','encoding_ns')};v=metrics['ns'];passed=abs(st.median(v))<=2 and max(map(abs,v))<=5
   results[(scope,study,tuple(group))]=metrics
   lines.append(f'| {scope} / {study} | {m} × {d} / {mode} | {st.median(v):+.2f}% | {min(v):+.2f}% … {max(v):+.2f}% | {st.median(metrics["gpu_ns"]):+.2f}% | {st.median(metrics["encoding_ns"]):+.2f}% | {("pass" if passed else "fail") if study=="aa" else "—"} |')
  assert load(name.replace('.json','-load.json'))['samples']
lines+=['','Positive reduction means less elapsed time. GPU command timestamps include stalls and scheduling; they are not exclusive arithmetic. Encoding scopes overlap and are not additive with GPU time. Instrumented within-runtime evidence is distinct from the uninstrumented frozen-production comparisons below.','','## Uninstrumented frozen production comparison','','Baseline is frozen accepted `dd940e07`; candidate uses the same shaders, compiler-generated program and task archive. No timing counters are enabled. Fresh-process adjacent ABBA/BAAB blocks use matching iterations, two warm-up rounds and three measured rounds per process, targeting one second per batch (8,192-call cap). Every per-invocation allocation, transfer, execution and cleanup stays inside timing. AA uses the identical enabled candidate on both sides. Uncached compares frozen production with private reuse disabled, preserving upload caching.','','| Study | Rows × width / mode | Time reduction | Block range |','|---|---|---:|---:|']
for study in ('aa','production','repeat','uncached'):
 name=f'gpu-prepared-output-priority-frozen-{study}-2026-10-29.json';data=load(name);identity(data)
 assert data['qualification_sha256']==p.sha(B/'qualification.json')
 for obs in data['observations']:p.observations('\n'.join(json.dumps(x) for x in obs['samples']),obs['cell'][1]*obs['cell'][2]*4,obs['iterations'],3)
 for group in {tuple(x['cell'][1:]) for x in data['observations']}:
  for field in ('attempts','dispatches','submissions'):
   assert len({x[field]/obs['iterations'] for obs in data['observations'] if tuple(obs['cell'][1:])==group for x in obs['samples']})==1
 for pair in data['pairs']:
  values={'baseline':[],'current':[]}
  for obs in data['observations']:
   if obs['process']==pair['block'] and obs['cell'][1:]==pair['group']:values[obs['cell'][0]].append(st.median(x['ns']/obs['iterations'] for x in obs['samples'] if x['round']>=0))
  assert len(values['baseline'])==len(values['current'])==2
  before=st.mean(values['baseline']);after=st.mean(values['current'])
  assert abs(pair['baseline_ns']-before)<.001 and abs(pair['current_ns']-after)<.001 and abs(pair['time_reduction']-(1-after/before))<1e-12
 for group in sorted({tuple(x['group']) for x in data['pairs']}):
  v=[x['time_reduction']*100 for x in data['pairs'] if tuple(x['group'])==group]
  lines.append(f'| {study} | {group[0]} × {group[1]} / {group[2]} | {st.median(v):+.2f}% | {min(v):+.2f}% … {max(v):+.2f}% |')
 for obs in data['cache_observations']:
  for c in obs['samples']:
   assert c['retained_bytes']==c['live_bytes']+c['cached_bytes'] and c['cached_bytes']<=16*1024*1024 and c['peak_retained_bytes']<=64*1024*1024
   maxidle=max(maxidle,c['cached_bytes']);maxretained=max(maxretained,c['peak_retained_bytes'])
 assert load(name.replace('.json','-load.json'))['samples']
lines+=classlines
lines+=['','## Resource tradeoff and limits','',f'Maximum observed idle retention: {maxidle:,} bytes; retained payload: {maxretained:,} bytes, below the 64 MiB default. Driver/metadata memory is outside this pre-existing payload accounting. Live allocations evict idle entries when needed. Disable private retention or trim explicitly when idle memory matters.','','The [rejected first production policy](gpu-prepared-output-initial-2026-10-29.md) retains the upload working-set regression. The revised pool never admits private backing by displacing cached uploads; upload admission and live-budget pressure prefer private eviction. The earlier [same-runtime diagnostic](gpu-private-inprocess-2026-10-29.md) retains failed small-workload controls, replay-cleanup evidence, the reported-DVFS correlation and prototype gains. Those numbers are not substituted for this production candidate. All unfavorable blocks remain. CPU load snapshots cannot prove GPU idleness; measurements run sequentially without local builds, tests, profiling or plotting. Dispatch, submission and logical allocation-attempt counts are unchanged within each matched workload. No vendor numerical library is used in production. Qualification precedes the implementation commit: each record’s revision is the checkout HEAD, while source/artifact hashes and worktree status identify the actual uncommitted candidate that ran. The auditor checks measured source identities, using preserved snapshots for superseded candidates.']
lines+=['','## Interpretation and next experiment','','Resident512×512 improves 11.97% and 12.48% against frozen accepted production, with passing large-case controls. The corresponding same-runtime reductions are 13.08% and 14.00%. Failed smaller-workload controls prevent a broad small-call claim.','','Upload protection restores seven upload hits and zero misses, but this workload records no private hits and eight private misses per call. Its frozen-production cohorts still show 4.08% and 2.68% median slowdowns; the cache-disabled candidate is approximately flat (+0.12%). These are separate cohorts and must not be subtracted as a causal estimate. The profiled repeat also tracks GPU interval variation, so bookkeeping is not established as the sole cause.','','Next remove unnecessary locking/scanning when the private pool is empty, and release private backing without taking the cache lock when a conservative pressure snapshot already proves it cannot be retained. This is a new candidate, requiring fresh qualification and performance evidence; these numbers are retained rather than substituted for the final runtime.']
(R/'gpu-prepared-output-priority-2026-10-29.md').write_text('\n'.join(lines)+'\n')
print('Audited production correctness, resources and all paired/control/replication samples.')
