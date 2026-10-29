"""Audit accepted-runtime host attribution and private-output feasibility controls."""
import json,statistics as st
from pathlib import Path
import protocol as p
R=p.ROOT/'benchmarks/results';B=p.ROOT/'benchmarks/.build/gpu-private-output-probe-2026-10-29'
def load(path):return json.loads(path.read_text())
def identity(data):
 for field in ('source_sha256','artifact_sha256','signature','artifacts'):
  for name,digest in data.get(field,{}).items():assert p.sha(p.ROOT/name)==digest,name
profile=load(R/'gpu-reuse-host-profile-2026-10-29.json');identity(profile)
assert len(profile['checks'])==24 and all(x['bit_identical'] for x in profile['checks'])
verified=load(R/'gpu-reuse-host-verification-2026-10-29.json');identity(verified)
assert verified['profile_sha256']==p.sha(R/'gpu-reuse-host-profile-2026-10-29.json')
assert len(verified['checks'])==24 and all(x['passed'] for x in verified['checks'])
assert len(verified['oracles'])==8 and all(x['metrics']['max_error_bound_ratio']<=1 for x in verified['oracles'])
q=load(B/'qualification.json');identity(q)
assert len(q['checks'])==72 and sum(x['comparisons'] for x in q['checks'])==288 and all(x['passed'] for x in q['checks'])
assert len(q['oracles'])==8 and all(x['metrics']['max_error_bound_ratio']<=1 for x in q['oracles'])
identity(load(B/'manifest.json'))
lines=['# Resident host attribution and private-output feasibility — 29 October 2026','','Production baseline: `dd940e07`. These tools operate on copied runtime sources; production stays unchanged. No numerical or cancellation contract change is accepted here.','','## Host attribution','','Instrumentation scopes are inclusive and overlap. Do not add them. Allocation counts are actual `allocateMetal` calls; destruction counts also include lightweight parameter-slice owners. Three processes, two warm-up and three measured 100-call rounds per case. Absolute times include instrumentation and prevailing host load. These are diagnostic observations, not a production speed comparison.','','| Resident batch × width | Wall µs | GPU µs | Allocate µs | Destroy µs | Allocate calls | Submit→GPU µs | GPU→wake µs | Budget CAS µs |','|---|---:|---:|---:|---:|---:|---:|---:|---:|']
for m,d in ((1,512),(16,512),(64,256),(64,512)):
 rows=[x for r in profile['observations'] if (r['m'],r['d'],r['mode'])==(m,d,0) for x in r['samples'] if x.get('host_profile') and x['round']>=0]
 med=lambda i:st.median(x['buckets'][i]/100/1000 for x in rows)
 lines.append(f'| {m} × {d} | {st.median(x["wall_ns"]/100/1000 for x in rows):.1f} | {med(8):.1f} | {med(0):.1f} | {med(1):.1f} | {st.median(x["buckets"][12]/100 for x in rows):.0f} | {med(6):.1f} | {med(7):.1f} | {med(18):.2f} |')
lines+=['','## Candidate and correctness','','The probe reuses only private Float32 outputs of validated pure prepared progression. Successful eligible maps/matrix operations overwrite every physical byte. Ordinary primitive execution, transforms, packed types, empty outputs, flags, scratch and parameters retain the accepted allocation path. Last-owner retirement with zero admissions is required; no pending resource is cached. Device, exact length and shared/private kind are distinct keys. A combined 16 MiB/64-entry idle pool remains inside the existing 64 MiB payload budget. In this copied diagnostic only, counters named upload include both kinds.','','288 changing-input full-output comparisons pass across both probe settings, three runner policies, three upload modes and four shapes; signed zero is preserved in decoding. Eight high-precision oracle checks pass. This does not replace production concurrency, held-ACK, failure-injection, lifetime and budget qualification.','','## Timing validity','','The original 250 ms identical-build control completed, with up to 44.6% absolute paired swings. The subsequent enabled screen was interrupted when the desktop app crashed. It has no complete result JSON and is not accepted evidence. Partial load data and logs are retained. The app crash report points into V8/Node buffer allocation and does not establish a causal link to GPU reuse. No surviving benchmark was found after restart.','','A separately declared post-restart confirmation measures only resident64×512 with five adjacent ABBA/BAAB blocks, a 1,000 ms adaptive target, 8,192-call ceiling, two warm-up and three measured rounds per process. AA uses the identical disabled probe on both sides; production compares frozen accepted production against the enabled probe. These are separate cohorts and must not be subtracted as a causal estimate.','','| Study | Batch × width / mode | Paired time reduction | Full block range |','|---|---|---:|---:|']
loads=[]
for study in ('aa','confirm-aa','confirm-production','broad-repeat'):
 data=load(R/f'gpu-private-output-{study}-2026-10-29.json');identity(data)
 assert data['qualification_sha256']==p.sha(B/'qualification.json')
 blocks=3 if study in ('aa','broad-repeat') else 5
 groups=12 if study=='aa' else 5 if study=='broad-repeat' else 1
 assert data['protocol']['blocks']==blocks and data['protocol']['target_ms']==(250 if study=='aa' else 1000)
 assert len(data['pairs'])==blocks*groups and len(data['observations'])==blocks*groups*4
 for obs in data['observations']:
  checked=p.observations('\n'.join(json.dumps(x) for x in obs['samples']),obs['cell'][1]*obs['cell'][2]*4,obs['iterations'],3)
 for pair in data['pairs']:
  before=[];after=[]
  for obs in data['observations']:
   if obs['process']!=pair['block'] or obs['cell'][1:]!=pair['group']:continue
   value=st.median(x['ns']/obs['iterations'] for x in obs['samples'] if x['round']>=0)
   (before if obs['cell'][0]=='baseline' else after).append(value)
  assert abs(pair['baseline_ns']-st.mean(before))<.001 and abs(pair['current_ns']-st.mean(after))<.001
  assert abs(pair['time_reduction']-(1-st.mean(after)/st.mean(before)))<1e-12
 for obs in data['cache_observations']:
  for x in obs['samples']:
   assert x['retained_bytes']==x['live_bytes']+x['cached_bytes'] and x['peak_retained_bytes']<=64*1024*1024 and x['cached_bytes']<=16*1024*1024
 for g in sorted({tuple(x['group']) for x in data['pairs']}):
  v=[x['time_reduction']*100 for x in data['pairs'] if tuple(x['group'])==g]
  lines.append(f'| {study} | {g[0]} × {g[1]} / {g[2]} | {st.median(v):+.2f}% | {min(v):+.2f}% … {max(v):+.2f}% |')
 loadrows=load(R/f'gpu-private-output-{study}-2026-10-29-load.json')['samples']
 loads.append(f'\nExternal CPU load for {study}: median {st.median(x["external_cpu_percent"] for x in loadrows):.1f}%, maximum {max(x["external_cpu_percent"] for x in loadrows):.1f}% (100% = one core). CPU sampling does not establish GPU idleness.\n')
odd=load(R/'gpu-private-output-odd-qualification-2026-10-29.json');identity(odd)
assert len(odd['checks'])==72 and sum(x['comparisons'] for x in odd['checks'])==288 and all(x['passed'] for x in odd['checks'])
assert len(odd['oracles'])==8 and all(x['metrics']['max_error_bound_ratio']<=1 for x in odd['oracles'])
lines+=['','## Interpretation','','The focused +10.86% resident64×512 result does not reproduce in the broader cohort: its three blocks include both a gain and substantial slowdowns. The candidate is not promoted to production. Improvements in another size remain leads for further investigation, not a general performance claim. Broader-case differences and all negative blocks remain retained.','','The additional odd/small-shape qualification passes 288 full-output comparisons and eight independent oracles on 3×33, 7×65, 1×17 and 31×31. Combined probe qualification is 576 full comparisons and 16 independent oracle checks. These do not establish lifetime/concurrency qualification or erase the failed performance replication.','','Next isolate allocation/residency from driver and scheduling variance using paired intervals within one long-lived runtime, retaining identical-build controls and GPU/host timing counters. Keep production at the accepted shared-upload cache until repeated broad measurements and ownership/fault/cancellation gates justify another change.','','## External load']+loads
lines+=['','## Reproduction','','`reuse_host_profile.py --frozen ACCEPTED --build NEW --output NEW.json` builds a diagnostic copy. `private_output_probe.py --phase prepare|verify --frozen ACCEPTED --build PROBE --output NEW.json` prepares and verifies the reuse probe. `private_output_confirm.py --phase measure --study aa|production --frozen ACCEPTED --build PROBE --output NEW.json` applies the focused confirmation. Never overlap measurements with compilation, tests, profiling or another benchmark. Frozen local source/artifact manifests and numerical qualification are retained; large binaries remain ignored. Run this auditor to verify hashes and paired observations before interpreting the report.']
(R/'gpu-private-output-2026-10-29.md').write_text('\n'.join(lines)+'\n')
print('Audited host attribution, 576 probe outputs, 16 probe oracles, signed-zero profile checks and all paired cohorts.')
