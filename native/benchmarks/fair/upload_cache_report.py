"""Audit frozen production upload-cache qualification and paired/control observations."""
import json,statistics as st
from pathlib import Path
import protocol as p
R=p.ROOT/'benchmarks/results'
B=p.ROOT/'benchmarks/.build/gpu-upload-cache-2026-10-29'
med=st.median

def identity(data):
 for field in ('source_sha256','artifact_sha256'):
  for name,expected in data[field].items():
   actual=p.ROOT/name
   if field=='source_sha256' and (not actual.exists() or p.sha(actual)!=expected):
    actual=next((folder/'frozen-sources'/name for folder in (B,B.with_name('gpu-upload-cache-atomic-2026-10-29')) if (folder/'frozen-sources'/name).exists() and p.sha(folder/'frozen-sources'/name)==expected),actual)
   assert actual.exists() and p.sha(actual)==expected,name

def audit(data):
 identity(data)
 expected={tuple(r['cell']):r['expected'] for r in data['summary']};groups={}
 for row in data['calibration']+data['observations']:
  cell=tuple(row['cell'])
  p.observations('\n'.join(json.dumps(v) for v in row['samples']),expected[cell],row['iterations'],data['protocol']['rounds'])
  if 'process' in row:
   groups.setdefault((cell[1:],row['process']),{}).setdefault(cell[0],[]).append(med(s['ns']/row['iterations'] for s in row['samples'] if s['round']>=0))
 assert len(data['observations'])==4*len(data['pairs'])==4*(1 if 'resident_confirmation' in data else 5 if 'settlement' in data or ('atomic_followup' in data and data['study']=='aa') else 12)*data['protocol']['blocks']
 for row in data['pairs']:
  g=groups[tuple(row['group']),row['block']];assert len(g['baseline'])==len(g['current'])==2
  before,after=st.mean(g['baseline']),st.mean(g['current'])
  assert before==row['baseline_ns'] and after==row['current_ns'] and 1-after/before==row['time_reduction']
 assert data['qualification_sha256']==p.sha(R/('gpu-upload-cache-atomic-qualification-2026-10-29.json' if 'atomic_followup' in data else 'gpu-upload-cache-qualification-2026-10-29.json'))
 for obs in data['cache_observations']:
  version,m,d,mode=obs['cell'];old=version=='baseline' and data['study'] in ('paired','repeat','uncached')
  cap=16*1024*1024 if version=='current' and data['study'] not in ('aa','uncached') else 0
  assert len(obs['samples'])==(0 if old else 5)
  for sample in obs['samples']:
   assert sample['retained_bytes']==sample['live_bytes']+sample['cached_bytes']
   assert sample['peak_retained_bytes']<=64*1024*1024 and sample['cached_bytes']<=cap
   if not cap or mode==0:assert sample['hits']==sample['cached_bytes']==0
   elif sample['round']>=0:assert sample['hits']>0

q=json.loads((R/'gpu-upload-cache-qualification-2026-10-29.json').read_text());identity(q)
assert len(q['checks'])==108 and sum(r['comparisons'] for r in q['checks'])==432
assert all(r['passed'] for r in q['checks']) and len(q['oracles'])==8
assert all(r['metrics']['max_error_bound_ratio']<=1 for r in q['oracles'])
qualification=json.loads((R/'gpu-upload-cache-tests-2026-10-29.json').read_text());identity(qualification)
assert qualification['runtime_tests']==206 and qualification['completed_cli_tests']==2
studies={}
for study,blocks in [('paired',7),('aa',5),('repeat',7),('toggle',5),('uncached',3)]:
 data=json.loads((R/f'gpu-upload-cache-{study}-2026-10-29.json').read_text());audit(data)
 assert data['protocol']['blocks']==blocks
 studies[study]=data

def rows(study,group):return [r for r in studies[study]['pairs'] if tuple(r['group'])==group]
def reduction(study,group):return med(r['time_reduction']*100 for r in rows(study,group))
groups=sorted({tuple(r['group']) for r in studies['paired']['pairs']})
lines=['# First bounded shared-upload cache candidate — 29 October 2026','',
'**Not promoted:** longer-batch confirmation found a 7.92% resident 64×256 regression. The atomic-ledger follow-up is documented separately. This first candidate is compared against the frozen pre-cache production executable (`c542d414`). Apple M2 Max, macOS 27. The generated transformer shaders, task core and arithmetic are unchanged. This replaces the earlier diagnostic allocator with a per-runtime cache whose idle storage is charged against the existing payload budget.','',
'## Qualification and resource contract','',
'206 runtime/support CTests passed across debug, optimized, address/undefined-sanitized and thread-sanitized builds. Two complete all-type matrix CLI suites also passed under ThreadSanitizer. The original 339-test selection was deliberately interrupted after those two CLI suites: the remaining repeated frontend CLI compilation was excluded because compiler/shader generation is unchanged. This is not a claim that the full compiler suite ran. The retained logs and selection record distinguish completed from interrupted work.','',
'New tests cover changing data, immutable aliases, exact budgets and idle eviction, byte/entry limits, warm allocation-failure injection, partial-copy cancellation/deadlines, held GPU completion, concurrent callers and trimming, plus disabled/private-storage controls. Separately, 432 complete decoded-float32 output comparisons cover four shapes, three upload modes, three runner policies, three cache limits and four alternating-input calls. Eight untouched-production outputs pass the independent high-precision forward-error oracle. Metal and shader validation are enabled for these numerical checks and disabled for timings.','',
'The default cache is limited to 16 MiB and 64 exact-size/device entries, inside `maxBytes` (64 MiB by default). Only CPU-populated shared upload backing is eligible. Every logical byte and sub-word padding is rewritten before publication. Private outputs, scratch, staging and parameter arenas are excluded. Last-owner retirement requires zero admitted operations; concurrent/potentially pending resources are discarded. Ordinary owners pin resources through GPU acknowledgement. Idle entries are evicted to satisfy new requests; runtime destruction closes and trims the cache. No new cancellation checkpoint, numerical relaxation or pending-buffer reuse is introduced.','',
'Existing live-owner statistics retain their meaning; new retained/cache statistics expose idle memory. This deliberately trades bounded idle retention for allocation reuse. `maxCachedUploadBytes=0` disables it; `trimUploadCache()` releases idle backing. Concurrent statistic reads are not a transaction. Driver bookkeeping and control-object allocations remain outside the payload-byte budget, as before.','',
'## Frozen paired measurements','',
'Seven ABBA/BAAB blocks for the original and independent repeat, five for identical-build and cache-off/on controls, and three comparing the old production executable with the new runtime with caching disabled. Two warm-up and three measured rounds per fresh process; adaptive 150 ms target, at most 512 calls. Setup is excluded; upload modes include per-invocation copies. All samples and adverse results remain retained. Positive percentages mean less elapsed time. Median paired percentages need not equal the ratio of independently pooled timings.','',
'| Batch × width | Upload mode | Repeat old → new µs | First reduction | Repeat reduction | Identical-build control | Same-build off → on | Old → new disabled |',
'|---|---|---:|---:|---:|---:|---:|---:|']
for group in groups:
 m,d,mode=group;r=rows('repeat',group)
 lines.append(f'| {m} × {d} | {("resident","input","all weights")[mode]} | {med(x["baseline_ns"] for x in r)/1000:.1f} → {med(x["current_ns"] for x in r)/1000:.1f} | '+' | '.join(f'{reduction(s,group):+.2f}%' for s in ('paired','repeat','aa','toggle','uncached'))+' |')
lines+=['','The resident cases cannot benefit directly from upload-cache hits. Their results measure the full runtime change, including ledger synchronization and timing variation. The same-build off/on comparison separates cache reuse from the rest of that change; the identical-build control estimates observed measurement variation. Do not subtract percentages from independent cohorts as a causal estimate.','',
'### Between-block variation','',
'Full minimum–maximum paired reductions, without removing outliers:','',
'| Batch × width | Mode | First range | Repeat range | Off/on range |','|---|---|---:|---:|---:|']
for group in groups:
 m,d,mode=group
 ranges=[]
 for study in ('paired','repeat','toggle'):
  values=[r['time_reduction']*100 for r in rows(study,group)]
  ranges.append(f'{min(values):+.2f}% … {max(values):+.2f}%')
 lines.append(f'| {m} × {d} | {("resident","input","all weights")[mode]} | '+' | '.join(ranges)+' |')
settled={}
for study in ('aa','repeat'):
 data=json.loads((R/f'gpu-upload-cache-settled-{study}-2026-10-29.json').read_text());audit(data)
 assert data['protocol']['blocks']==3 and data['protocol']['target_ms']==1000 and data['protocol']['max_iterations']==8192
 settled[study]=data
lines+=['','## Longer-batch confirmation','','The original cohorts remain above, including the 45.63% batch64/input slowdown and the very large identical-build swings. These prompted a separate, predeclared five-case follow-up: three paired blocks, 1,000 ms adaptive target, maximum 8,192 calls, unchanged frozen binaries and hot loop. Identical-build control runs before the production comparison. No original samples are removed.','','| Batch × width | Mode | Old → new µs | Paired reduction | Full block range | Identical-build control |','|---|---|---:|---:|---:|---:|']
for group in sorted({tuple(r['group']) for r in settled['repeat']['pairs']}):
 before=[r for r in settled['repeat']['pairs'] if tuple(r['group'])==group]
 control=[r['time_reduction']*100 for r in settled['aa']['pairs'] if tuple(r['group'])==group]
 changes=[r['time_reduction']*100 for r in before];m,d,mode=group
 lines.append(f'| {m} × {d} | {("resident","input","all weights")[mode]} | {med(r["baseline_ns"] for r in before)/1000:.1f} → {med(r["current_ns"] for r in before)/1000:.1f} | {med(changes):+.2f}% | {min(changes):+.2f}% … {max(changes):+.2f}% | {med(control):+.2f}% |')
cache=[r for data in studies.values() for obs in data['cache_observations'] for r in obs['samples']]
lines+=['',f'Maximum observed idle cache: {max(r["cached_bytes"] for r in cache):,} bytes. Maximum observed retained payload: {max(r["peak_retained_bytes"] for r in cache):,} bytes, below the 67,108,864-byte budget. These are observed peaks for this workload set, not a new budget guarantee beyond the enforced cap.','',
'## Load and scope','',
'No builds, tests or profilers overlap acceptance. CPU process sampling is retained and does not prove GPU idleness. The task’s Xcode capture windows were closed before this milestone.','',
'| Study | External CPU median | External CPU maximum |','|---|---:|---:|']
for study in [*studies,'settled-aa','settled-repeat']:
 load=json.loads((R/f'gpu-upload-cache-{study}-2026-10-29-load.json').read_text());cpu=[r['external_cpu_percent'] for r in load['samples']]
 lines.append(f'| {study} | {med(cpu):.1f}% | {max(cpu):.1f}% |')
lines+=['','100% means one CPU core. Results cover these transformer workloads on one M2 Max; they do not establish performance on other GPUs, concurrent service workloads or genuinely cold driver caches. Production uses no Accelerate/MPSGraph numerical implementation.','',
'Run `python3 native/benchmarks/fair/upload_cache_report.py` to audit the retained local frozen artifacts, recompute every paired ratio and regenerate this report. Raw JSON records contain commands, source/artifact hashes, all calibration/warm-up/measured observations and cache statistics. Large binaries and generated shader bundles remain ignored under `benchmarks/.build`.']
(R/'gpu-upload-cache-2026-10-29.md').write_text('\n'.join(lines)+'\n')
print('Audited 432 full outputs, 208 completed CTests and all five initial and two longer-batch paired studies.')
