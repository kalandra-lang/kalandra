"""Audit production prepared-output cache correctness, caps and every paired sample."""
import json,statistics as st
from pathlib import Path
import protocol as p
R=p.ROOT/'benchmarks/results';B=p.ROOT/'benchmarks/.build/gpu-prepared-output-cache-2026-10-29'
def load(name):return json.loads((R/name).read_text())
def identity(data):
 for field in ('source_sha256','artifact_sha256','signature','artifacts','sources'):
  for name,digest in data.get(field,{}).items():
   assert any(x.is_file() and p.sha(x)==digest for x in [p.ROOT/name,B/'frozen-sources'/name]),name
q=load('gpu-prepared-output-qualification-2026-10-29.json');identity(q)
assert p.sha(B/'qualification.json')==p.sha(R/'gpu-prepared-output-qualification-2026-10-29.json')
assert len(q['checks'])==486 and sum(x['comparisons'] for x in q['checks'])==1944 and all(x['passed'] for x in q['checks'])
assert {x['cap'] for x in q['checks']}=={0,4096,16*1024*1024}
assert len(q['oracles'])==18 and all(x['metrics']['max_error_bound_ratio']<=1 for x in q['oracles'])
for name in ('gpu-prepared-output-manifest-2026-10-29.json','gpu-prepared-output-frozen-manifest-2026-10-29.json'):identity(load(name))
tests=load('gpu-prepared-output-test-record-2026-10-29.json');identity(tests)
assert tests['tests']==201 and not tests['failed']
assert tests['log_sha256']==p.sha(R/'gpu-prepared-output-tests-2026-10-29.txt')
lines=['# Prepared-output reuse: rejected first eviction policy — 29 October 2026','','Production runtime extends the accepted per-runtime upload cache with completed, fully overwritten Float32 prepared outputs. One combined 16 MiB / 64-entry idle cap remains inside the 64 MiB default payload budget. Last-owner retirement, zero admissions, fresh metadata and distinct shared/private keys preserve immutable ownership. No numerical, cancellation, checkpoint or shader change. Native C++ clients must rebuild for the extended Options/Stats layout.','','## Correctness and scope','','201 GPU runtime/support tests pass, including prepared-program integration in debug, optimized, ASan/UBSan and TSan modes. The dedicated fixture checks aliases, opposite prior bits (including NaNs/negative zero), changing ranks, storage-kind separation, exact budget pressure, all allocation-failure points, held acknowledgement, cancellation/deadlines, device failure, concurrency, policy changes, entry caps and teardown. This is not a full compiler/CLI suite rerun.','','1,944 full-output comparisons pass against frozen accepted production across nine shapes, all three runner/upload modes, both switch orders and zero / 4 KiB / 16 MiB cache limits. Eighteen independent high-precision oracle checks pass. Signed zero survives decoding. Numerical comparisons precede timing and never overlap it.','','## Same-runtime paired intervals','','A single runtime, queue, prepared program and resident inputs span ABBA/BAAB intervals. Each slot trims idle backing and warms two complete intervals outside timing; all per-call execution remains timed. Five blocks, 1,024 calls per measured slot, two initial warm-up slots. GPU/encoding counters are enabled equally on both sides. AA disables private caching on both sides; toggle and repeat switch it on only for the current side. The original precision requirement is absolute AA wall median ≤2% and every block within ±5%; a failure limits the corresponding gain claim.','','| Scope / study | Rows × width / upload mode | Wall reduction | Block range | GPU interval reduction | Encoding reduction | AA precision |','|---|---|---:|---:|---:|---:|---|']
maxidle=maxretained=0;results={};classlines=['','## Cache class behavior','','Counters below are per measured call. Idle byte counts are post-interval snapshots, not peak live working sets. Both classes share the same cap.','','| Scope / study | Rows / mode | Private enabled | Shared hits / misses | Private hits / misses | Shared / private idle bytes |','|---|---|---|---:|---:|---:|']
for scope in ('largest','guards'):
 for study in ('aa','toggle','repeat'):
  name=f'gpu-prepared-output-{scope}-{study}-2026-10-29.json';data=load(name);identity(data)
  assert data['qualification_sha256']==p.sha(B/'qualification.json')
  assert data['protocol']==dict(blocks=5,iterations=1024,per_slot_warmup_intervals=2,initial_warmups=2,order='ABBA/BAAB',profile=True)
  assert len(data['observations'])==(1 if scope=='largest' else 3)
  for obs in data['observations']:
   group=obs['group'];m,d,mode=group;rows=obs['samples'];count=obs['iterations'];assert count==1024
   timed=[x for x in rows if 'ns' in x];cache=[x for x in rows if x.get('cache')];states=[x for x in rows if x.get('pair_state')];kinds=[x for x in rows if x.get('cache_kinds')]
   p.observations('\n'.join(json.dumps(x) for x in timed),m*d*4,count,20)
   assert len(timed)==len(cache)==len(states)==len(kinds)==22
   for sample,c,state,k in zip(timed,cache,states,kinds):
    assert sample['round']==c['round']==state['round']==k['round']
    assert sample['gpu_ns']>0 and sample['encoding_ns']>0
    assert c['retained_bytes']==c['live_bytes']+c['cached_bytes'] and c['cached_bytes']==k['shared_bytes']+k['private_bytes']
    assert c['cached_bytes']<=16*1024*1024 and c['peak_retained_bytes']<=64*1024*1024
    if not state['enabled']:assert not k['private_bytes'] and not k['private_hits']
    if mode==0:assert bool(k['private_bytes'])==state['enabled']
    if mode==2:
     assert k['shared_hits']==count*(6 if state['enabled'] else 7) and k['shared_misses']==count*int(state['enabled'])
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
lines+=classlines
lines+=['','## Rejected first policy','','The large resident control passes (0.94% median drift, every block within ±3.83%). Enabled reuse reduces elapsed time 13.48%, independently repeated at 13.93%. Small-workload controls remain noisy and do not support small-call claims.','','The upload-heavy case regresses 2.09% in the first cohort and 3.29% in replication. Every block in both cohorts is slower. Class counters expose one upload miss per call (six hits), replacing the accepted seven upload hits. Four private hits do not compensate. The unchanged 16 MiB combined cap is bounded but the eviction policy lets private retention displace an upload working set. This policy is rejected; the next candidate protects existing uploads and prefers private eviction. All controls, numerical qualification, tests and negative samples remain retained. A [versioned reconstruction patch](../../native/benchmarks/prepared-output-initial-policy.patch) restores this rejected source in an isolated copy; frozen local sources remain the original identity reference.','','The uninstrumented comparator was prepared but never measured: its waiting coordinator was stopped before launch after this regression was identified. Do not infer any frozen-production performance from its manifest.','','Correctness passes 201 runtime/support tests and 1,944 full comparisons plus 18 oracles; this performance rejection does not imply numerical failure. No numerical, checkpoint, ownership or cancellation contract changes.']
(R/'gpu-prepared-output-initial-2026-10-29.md').write_text('\n'.join(lines)+'\n')
print('Audited rejected first cache policy, all six paired cohorts and numerical/resource qualification.')
