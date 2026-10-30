"""Audit final bounded output reuse, conservative fast misses and frozen comparisons."""
import json,statistics as st,itertools
import protocol as p
R=p.ROOT/'benchmarks/results';B=p.ROOT/'benchmarks/.build/gpu-prepared-output-fastmiss-cache-2026-10-29'
def load(name):return json.loads((R/name).read_text())
def identity(data):
 for field in ('source_sha256','artifact_sha256','signature','artifacts','sources'):
  for name,digest in data.get(field,{}).items():assert p.sha(p.ROOT/name)==digest,name
q=load('gpu-prepared-output-fastmiss-qualification-2026-10-29.json');identity(q)
assert p.sha(B/'qualification.json')==p.sha(R/'gpu-prepared-output-fastmiss-qualification-2026-10-29.json')
shapes={(1,512),(16,512),(64,512),(64,256),(3,33),(7,65),(1,17),(31,31),(512,512)}
expected={(m,d,cap,rev,policy,mode) for (m,d),cap,rev,policy,mode in itertools.product(shapes,(0,4096,16*1024*1024),(False,True),(0,1,2),(0,1,2))}
assert {(x['m'],x['d'],x['cap'],x['reverse'],x['policy'],x['mode']) for x in q['checks']}==expected
assert len(q['checks'])==486 and sum(x['comparisons'] for x in q['checks'])==1944 and all(x['passed'] for x in q['checks'])
assert {(x['m'],x['d'],x['pattern']) for x in q['oracles']}=={(m,d,pattern) for m,d in shapes for pattern in (0,1)}
assert len(q['oracles'])==18 and all(x['metrics']['max_error_bound_ratio']<=1 for x in q['oracles'])
for name in ('manifest','frozen-manifest'):identity(load(f'gpu-prepared-output-fastmiss-{name}-2026-10-29.json'))
tests=load('gpu-prepared-output-fastmiss-test-record-2026-10-29.json');identity(tests)
assert tests['tests']==201 and not tests['failed'] and tests['log_sha256']==p.sha(R/'gpu-prepared-output-fastmiss-tests-2026-10-29.txt')
lines=['# Qualified bounded prepared-output reuse — 29–30 October 2026','','The runtime reuses completed, fully overwritten Float32 backing of validated pure prepared progression. A combined 16 MiB / 64-entry idle pool stays inside the existing 64 MiB default payload budget. Uploads have priority over private retention; scratch, transforms, flags, packed types and parameters keep their existing paths. No arithmetic, shader, checkpoint or cancellation-contract change. Native C++ clients must rebuild for the extended Options/Stats layout.','','An atomic empty-private-pool snapshot skips pointless locking/scanning. A positive upload-pressure snapshot may conservatively discard private backing without locking; admission still rechecks under the mutex. Races can lose an optional cache hit, but cannot reuse pending backing or overspend the existing atomic budget. Logical allocation permits are unchanged.','','## Qualification','','201 GPU runtime/support tests pass, including prepared-program integration in debug, optimized, ASan/UBSan and TSan modes. Coverage includes aliases, opposite old contents (NaNs/negative zero), ranks, exact budgets, all allocation failures, held ACK, cancellation/deadlines/device failure, concurrent cores, policy changes, byte/entry caps and teardown. This is not a full compiler/CLI-suite rerun.','','Fresh qualification passes 1,944 changing-input full-output comparisons against frozen accepted production across nine shapes, three cache caps, both switching orders and every runner/upload mode. Eighteen independent high-precision oracle checks pass. Signed zero is preserved. Qualification completes before timing.','','## Uninstrumented frozen comparisons','','Frozen baseline is accepted `dd940e07`; both sides use the same shaders, compiler-generated program, task archive, source algorithm and complete per-call lifecycle. No timing counters are enabled. Mode 0 keeps operands resident; mode 2 uploads every operand on each invocation (host tensors are prepared once equally on both sides). Three adjacent ABBA/BAAB process blocks, two warm-up and three measured rounds, matching iterations, adaptive one-second target, 8,192-call cap. Setup is excluded equally; all per-invocation work remains timed. AA controls all four cases with the identical enabled candidate. Uncached compares frozen production against the new candidate with private reuse disabled and uploads retained.','','The predeclared precision goal is absolute AA median ≤2%, every block within ±5%. Failed controls limit corresponding claims. No sample is removed. Separate study medians must not be subtracted as causal estimates.','','| Study | Rows × width / mode | Time reduction | Block range | AA precision |','|---|---|---:|---:|---|']
maxidle=maxretained=0;summary={}
for study in ('aa','production','repeat','uncached'):
 name=f'gpu-prepared-output-fastmiss-frozen-{study}-2026-10-29.json';data=load(name);identity(data)
 assert data['qualification_sha256']==p.sha(B/'qualification.json') and data['protocol']['blocks']==3
 groups={tuple(x['cell'][1:]) for x in data['observations']};assert groups=={(1,512,0),(64,512,0),(512,512,0),(512,512,2)}
 for obs in data['observations']:p.observations('\n'.join(json.dumps(x) for x in obs['samples']),obs['cell'][1]*obs['cell'][2]*4,obs['iterations'],3)
 for group in groups:
  for field in ('attempts','dispatches','submissions'):
   ratios={x[field]/obs['iterations'] for obs in data['observations'] if tuple(obs['cell'][1:])==group for x in obs['samples']}
   assert len(ratios)==1
   assert next(iter(ratios))==0 if field=='dispatches' else next(iter(ratios))>0
 for pair in data['pairs']:
  values={'baseline':[],'current':[]}
  for obs in data['observations']:
   if obs['process']==pair['block'] and obs['cell'][1:]==pair['group']:values[obs['cell'][0]].append(st.median(x['ns']/obs['iterations'] for x in obs['samples'] if x['round']>=0))
  assert len(values['baseline'])==len(values['current'])==2
  before=st.mean(values['baseline']);after=st.mean(values['current'])
  assert abs(pair['baseline_ns']-before)<.001 and abs(pair['current_ns']-after)<.001 and abs(pair['time_reduction']-(1-after/before))<1e-12
 for group in sorted(groups):
  values=[x for x in data['pairs'] if tuple(x['group'])==group];v=[x['time_reduction']*100 for x in values];summary[(study,group)]=v
  passed=abs(st.median(v))<=2 and max(map(abs,v))<=5
  lines.append(f'| {study} | {group[0]} × {group[1]} / {group[2]} | {st.median(v):+.2f}% | {min(v):+.2f}% … {max(v):+.2f}% | {("pass" if passed else "fail") if study=="aa" else "—"} |')
 for obs in data['cache_observations']:
  for c in obs['samples']:
   assert c['retained_bytes']==c['live_bytes']+c['cached_bytes'] and c['cached_bytes']<=16*1024*1024 and c['peak_retained_bytes']<=64*1024*1024
   maxidle=max(maxidle,c['cached_bytes']);maxretained=max(maxretained,c['peak_retained_bytes'])
 assert load(name.replace('.json','-load.json'))['samples']
counters=load('gpu-prepared-output-fastmiss-counters-2026-10-29.json');identity(counters)
assert counters['qualification_sha256']==p.sha(B/'qualification.json')
assert {tuple(x['group']) for x in counters['observations']}=={(1,512,0),(64,512,0),(512,512,2)}
assert counters['protocol']['blocks']==1 and counters['protocol']['iterations']==128
for obs in counters['observations']:
 timed=[x for x in obs['samples'] if 'ns' in x]
 for field in ('attempts','dispatches','submissions'):
  ratios={x[field]/obs['iterations'] for x in timed};assert len(ratios)==1 and next(iter(ratios))>0
 kinds=[x for x in obs['samples'] if x.get('cache_kinds')];states=[x for x in obs['samples'] if x.get('pair_state')];count=obs['iterations']
 assert len(kinds)==len(states)==6
 if obs['group'][2]==2:
  for k in kinds:assert k['shared_hits']==count*7 and not k['shared_misses']
large=(512,512,0);uploads=(512,512,2)
for group in (large,uploads):
 aa=summary[('aa',group)];assert abs(st.median(aa))<=2 and max(map(abs,aa))<=5
assert all(x>0 for study in ('production','repeat') for x in summary[(study,large)])
assert all(st.median(summary[(study,uploads)])>=0 for study in ('production','repeat'))
lines+=['','## Interpretation','','The accepted result is targeted: resident512×512 takes 11.44% less elapsed time against frozen production, independently repeated at 11.32%. Every block improves 10.81–12.48%; the identical-build control is -0.08% and stays within ±0.78%. The cache-disabled comparison is -1.00%. These are hot-invocation measurements of the same GPU program, not a universal language or numerical-library speed claim.','','The upload-heavy guard changes from the preceding candidate’s negative medians to +3.06% / +6.04%, retaining seven upload hits and zero misses. Its block range is -2.27% to +12.17%; a passing AA control still spans -4.82% to +3.86%. The cache-disabled cohort also improves 5.07%, so these separate cohorts do not isolate private-cache benefit or establish that fast misses alone caused the timing difference. All outcomes remain retained.','','Smaller resident controls fail, despite positive candidate medians. No reliable small-call total-time gain or zero-overhead guarantee is claimed. Earlier reported-DVFS correlation does not establish sole causality for these observations. Next profile GPU command/kernel time and dataflow on the final runtime; no numerical or cancellation relaxation is implied.']
lines+=['','## Resources and provenance','',f'Maximum observed idle retention: {maxidle:,} bytes; retained payload: {maxretained:,} bytes, below the existing 64 MiB default. Metadata and driver-internal memory remain outside this payload accounting. Private disable/trim and a zero-capacity option remain available.','','A separate short counter diagnostic verifies seven upload hits / zero misses in the upload-heavy case. Its timings are not acceptance evidence. Submissions and logical allocation-attempt counts are positive and unchanged within every matched workload. Dispatch counters are disabled in uninstrumented runs; the separate instrumented counter diagnostic verifies positive, unchanged dispatch counts on both cache settings. All acceptance runs are sequential, without overlapping builds, tests or profiling. External CPU load is recorded and does not prove GPU idleness.','','The [first policy](gpu-prepared-output-initial-2026-10-29.md) and [upload-priority follow-up](gpu-prepared-output-priority-2026-10-29.md) retain earlier gains, failed controls and regressions. Versioned reconstruction patches restore both superseded sources in isolated copies. The [same-runtime diagnostic](gpu-private-inprocess-2026-10-29.md) retains replay cleanup, reported-DVFS evidence and prototype observations; none replace final-runtime results. No production Accelerate/MPSGraph dependency. Source/artifact hashes identify the candidate qualified before committing; the checkout revision alone names its parent.']
(R/'gpu-prepared-output-fastmiss-2026-10-29.md').write_text('\n'.join(lines)+'\n')
print('Audited final fast-miss runtime, full numerical/resource qualification and every frozen comparison.')
