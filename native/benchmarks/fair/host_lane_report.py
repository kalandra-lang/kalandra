"""Audit and regenerate the host attribution / operand-sharing / reuse report."""
import csv,json,statistics as st
from pathlib import Path
import protocol as p
R=p.ROOT/'benchmarks/results'
load=lambda name:json.loads((R/name).read_text())
med=st.median

def audit(d):
 for field in ('source_sha256','artifact_sha256'):
  for name,h in d[field].items():
   path=p.ROOT/name
   assert path.exists() and p.sha(path)==h,name

 if 'protocol' in d:
  expected={tuple(r['cell']):r['expected'] for r in d['summary']};groups={}
  for r in d['observations']:
   cell=tuple(r['cell']);p.observations('\n'.join(json.dumps(v) for v in r['samples']),expected[cell],r['iterations'],d['protocol']['rounds'])
   groups.setdefault((cell[1:],r['process']),{}).setdefault(cell[0],[]).append(med(v['ns']/r['iterations'] for v in r['samples'] if v['round']>=0))
  assert len(d['observations'])==4*len(d['pairs'])
  for r in d['pairs']:
   g=groups[(tuple(r['group']),r['block'])];assert len(g['baseline'])==len(g['current'])==2
   before,after=st.mean(g['baseline']),st.mean(g['current'])
   assert before==r['baseline_ns'] and after==r['current_ns'] and 1-after/before==r['time_reduction']

def reductions(d):
 return {g:med(x['time_reduction']*100 for x in d['pairs'] if tuple(x['group'])==g) for g in sorted({tuple(x['group']) for x in d['pairs']})}

host=load('gpu-aligned-host-profile-2026-10-29.json');audit(host);assert len(host['checks'])==12 and all(x['bit_identical'] for x in host['checks'])
assert len(host['observations'])==36
profiles={};residual=[]
for row in host['observations']:
 n=row['iterations'];hp=[s for s in row['samples'] if s.get('host_profile') and s['round']>=0]
 regular=[s for s in row['samples'] if not s.get('host_profile') and s['round']>=0]
 assert len(hp)==len(regular)==3
 for h in hp:
  v=h['buckets'];assert v[9]==n;residual.append(abs(v[6]+v[7]+v[8]-v[10]-v[11]))
 values=[med(x['wall_ns']/n/1000 for x in hp),*[med(x['buckets'][i]/n/1000 for x in hp) for i in (8,0,1,6,7)],med(x['encoding_ns']/n/1000 for x in regular)]
 profiles.setdefault((row['m'],row['d'],row['mode']),[]).append(values)
assert max(residual)<1000
lines=['# GPU host costs, SIMD sharing and backing-reuse probe — 29 October 2026','',
'Production remains at `efbe33f`. This study rejects the operand-sharing kernels and tests an explicitly different, diagnostic-only buffer-retention policy. No language, cancellation, arithmetic, ownership or production memory-budget contract changes are promoted.','',
'## Whole-production host attribution','',
'Apple M2 Max; copied current runtime, actual generated transformer bundle. Three fresh processes per shape/upload mode, two warm-ups plus three rounds of 100 calls. Each row is the median of process medians, in microseconds per call. These are instrumented diagnostics, not paired acceptance. Twelve full-output comparisons against the untouched production driver passed.','',
'| Batch × width | Upload mode | Wall | GPU | Allocation | Destruction | Submit → GPU start | GPU end → wake | Encoding |',
'|---|---|---:|---:|---:|---:|---:|---:|---:|']
for (m,d,mode),rows in sorted(profiles.items()):
 v=[med(r[i] for r in rows) for i in range(7)]
 lines.append(f'| {m} × {d} | {("resident","input","all weights")[mode]} | '+' | '.join(f'{x:.1f}' for x in v)+' |')
lines += ['',f'Allocation/destruction, encoding, progression and submit scopes overlap; **do not add them**. Submit/start/end/wake use the common Mach clock domain; the maximum arithmetic partition residual is {max(residual)} ns over a complete 100-call round. Submit-to-start includes driver/scheduling/residency effects and is not proof of any single cause. Profile overhead and prevailing system load affect absolute values. The host profile preceded the lane-sharing captures.', '',
'## Operand-sharing controls','',
'All versions preserve separately rounded products and the adjacent reduction tree. Complete tiles use explicit SIMD-group/lane attributes and require execution width 32; ragged cases retain the original 1D path. `mapping` changes only lane mapping. `a`/`b` share one operand, `both` shares both with indexed shuffles, and `fixed` uses constant broadcasts/XOR shuffles to test indexed-shuffle compiler expansion. Each version passes 84 numerical executions (420 total): every GPU output bit, input and guard is compared; independent CPU trees cover all cells up to 32,768 outputs and dispersed cells/corners above that. IEEE patterns check compatibility with the original kernel, not a new arithmetic contract.','',
'Apple documents physical thread-to-SIMD mapping as implementation-defined; the explicit mapping avoids relying on an assumed linear lane assignment. Sources: [threadgroups](https://developer.apple.com/documentation/metal/creating-threads-and-threadgroups), [Metal specification §6.10.2](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf).','',
'Xcode 27.1 compiler fields and exported counters, 64×2048×512. Resource values are shader/compiler evidence; replay durations are not acceptance timings. The baseline 94-register value comes from the preceding aligned-grid capture.','',
'| Mode | Registers / highest used | Spilled bytes | Measured occupancy | Dynamic ALU instructions | Shared-read limiter |',
'|---|---:|---:|---:|---:|---:|']
gui=load('gpu-lane-sharing-gui-2026-10-29.json')
for mode in ('mapping','a','b','both','fixed'):
 q=load(f'gpu-lane-{mode}-qualification-2026-10-29.json');audit(q);assert len(q['checks'])==84 and all(r['passed'] for r in q['checks'])
 row=next(x for x in gui['records'] if x['mode']==mode)
 counter=list(csv.DictReader((R/f'gpu-lane-{mode}-counters-2026-10-29.csv').open()))[0]
 lines.append(f"| {mode} | {row['allocated_registers']} / {row['high_register']} | {row['spilled_bytes']} | {counter['Kernel Occupancy']} | {int(counter['Kernel ALU Instructions']):,} | {counter['ThreadGroup/ImageBlock Read Limiter']} |")
lines += ['', 'Reducing the shared-read limiter merely moved the bottleneck: all sharing versions spill, with much more dynamic ALU work. Even the constant-lane control still spills. Mapping alone remains spill-free but is not a demonstrated improvement.', '',
'### Retained initial timing series — potentially contaminated','',
'The first series was initially believed to have stopped all replays. Closing the task capture windows later exposed a retained A-sharing Xcode GPU debug session. That session was explicitly stopped, then every task capture window was closed. All original files and load records remain retained, but **none of their timing ratios establish acceptance**. The per-file load note saying all replays were stopped was the belief at collection and is superseded here. These data must not be silently relabeled clean.', '',
'| Mode | Blocks | Median paired time reduction across the six shapes | External CPU median / max |','|---|---:|---:|---:|']
for mode in ('mapping','a','b','both','fixed'):
 x=load(f'gpu-lane-{mode}-paired-2026-10-29.json');audit(x);v=list(reductions(x).values());ls=load(f'gpu-lane-{mode}-paired-2026-10-29-load.json')['samples'];cpu=[r['external_cpu_percent'] for r in ls]
 lines.append(f"| {mode} | {x['protocol']['blocks']} | {min(v):+.2f}% to {max(v):+.2f}% | {med(cpu):.1f}% / {max(cpu):.1f}% |")
lines += ['', '### Repeated screens after closing capture windows','',
'No builds/tests/profiling overlap these runs. ABBA/BAAB fresh processes, two warm-up and three measured rounds, adaptive 30 ms target, maximum 256 calls. Negative reductions mean slower. Single-block repeats of severely losing variants are rejection checks, not precise acceptance estimates. CPU sampling does not prove GPU idleness.','',
'| M × K × N | Mapping | A | B | Both | Fixed |','|---|---:|---:|---:|---:|---:|']
repeats={}
for mode in ('mapping','a','b','both','fixed'):
 d=load(f'gpu-lane-{mode}-repeat-2026-10-29.json');audit(d);repeats[mode]=reductions(d)
for g in repeats['mapping']:
 lines.append('| '+' × '.join(map(str,g))+' | '+' | '.join(f'{repeats[m][g]:+.2f}%' for m in repeats)+' |')
lines += ['', '## Bounded backing-reuse diagnostic','',
'The same copied runtime binary runs with the cache disabled/enabled. The process-local cache is capped at 64 entries and 64 MiB, matches device/length/storage mode, and quarantines retired buffers until a driver boundary after a completed invocation proves no commands remain in flight. It never reuses a retired buffer inside the same invocation. The actual production shaders/task core remain unchanged.','',
'**This is not production-equivalent memory semantics.** Cached bytes are outside the current runtime live-byte ledger and the probe assumes one caller. Numerical correctness does not qualify cancellation, failure injection, concurrent invocations, budget eviction, or destruction of a production pool. Those require a deliberate runtime design. There is no production speedup claim here.', '',
'Changing inputs alternate patterns 0/1 over four calls, across four shapes, all three upload modes and all three runner policies: 144 decoded-float32 full-output comparisons with cache off/on (288 executions), plus eight untouched-production outputs and independent high-precision forward-error checks. Warm cache hits and the 64 MiB cap are asserted. An initial measurement harness incorrectly demanded hits in the first cold one-call warm-up; it stopped before collecting timings. The assertion was corrected to measured rounds, a new build was frozen, and all qualification cases rerun.', '',
'| Batch × width | Upload mode | Disabled µs | Enabled µs | Paired time reduction |','|---|---|---:|---:|---:|']
q=load('gpu-backing-reuse-qualification-2026-10-29.json');audit(q);assert len(q['checks'])==36 and all(x['bit_identical'] and x['changing_output_cases']==4 for x in q['checks'])
x=load('gpu-backing-reuse-paired-2026-10-29.json');audit(x)
for g,r in reductions(x).items():
 rows=[s for s in x['pairs'] if tuple(s['group'])==g];m,d,mode=g
 lines.append(f'| {m} × {d} | {("resident","input","all weights")[mode]} | {med(s["baseline_ns"] for s in rows)/1000:.1f} | {med(s["current_ns"] for s in rows)/1000:.1f} | {r:+.2f}% |')
lines += ['', '### Untouched-production executable controls', '', 'Three independent paired blocks per study compare the untouched production executable to the diagnostic with retention disabled (`off`), then enabled (`on`). The off control exposes instrumentation/driver changes; the on control measures the complete experimental policy against actual production. These remain screens under a different memory contract, not production acceptance. Both use the same numerical qualification and frozen bundle.', '', '| Batch × width | Upload mode | Off vs production | On vs production |', '|---|---|---:|---:|']
refs={}
for mode in ('off','on'):
 reference=load(f'gpu-backing-reference-{mode}-2026-10-29.json');audit(reference);refs[mode]=reductions(reference)
for g in refs['off']:
 m,d,mode=g
 lines.append(f'| {m} × {d} | {("resident","input","all weights")[mode]} | {refs["off"][g]:+.2f}% | {refs["on"][g]:+.2f}% |')
lines += ['', 'The strongest repeatable lead is all-weight upload at width 512: the independent enabled reductions are 54.74%/64.45% for batches 16/64, against disabled controls of 2.22%/0.32%. **Do not attribute the resident batch 64 result to caching:** disabled/enabled controls both improve by about 26%. Batch 16 resident is promising at 15.48%, but further controlled qualification is required. Batch 1 input regresses 3.25%; small resident/input gains are not established. These controls are separate cohorts, so their percentages must not be subtracted as a causal estimate.', '', 'External CPU samples (100% means one CPU core; this is not a GPU-idle measurement):', '', '| Study | Median CPU | Maximum CPU |', '|---|---:|---:|']
for name in ('gpu-backing-reuse-paired','gpu-backing-reference-off','gpu-backing-reference-on'):
 samples=load(name+'-2026-10-29-load.json')['samples'];cpu=[r['external_cpu_percent'] for r in samples]
 lines.append(f'| {name} | {med(cpu):.1f}% | {max(cpu):.1f}% |')
peaks=[s['peak_cached_bytes'] for obs in x['cache_observations'] for s in obs['samples']]
lines += ['',f'The main disabled/enabled screen uses seven paired blocks; adaptive 100 ms target, maximum 512 calls. Median paired ratios need not equal the ratio of separately pooled medians. Peak retained cache over these timings: {max(peaks):,} bytes. Setup is excluded and each fresh process warms its own cache. The untouched-production controls above provide an independent reference, but no identical-build control or production-compatible implementation has established an acceptance threshold for this policy.', '',
'## Next architectural decision','',
'Prioritize bounded completion-safe backing reuse over more shuffle micro-optimizations. A production design must charge live plus idle cached storage against the resource limit, evict idle entries before reporting budget exhaustion, keep pending GPU owners quarantined, handle concurrent invocation lifetimes, and define observable cache statistics/teardown and buffer-initialization guarantees. Preserve logical allocation-failure injection and cancellation/fault precedence. Start with deterministic ownership/budget tests and changing-input checks, then frozen paired/control/replication whole-model measurements. Do not adopt the diagnostic global cache as the implementation.', '',
'## Reproduction and evidence','',
'`aligned_host_profile.py --build DIR --output FILE` freezes an instrumented copy from the current aligned production artifacts. `lane_share_screen.py --mode mapping|a|b|both --phase prepare|capture|measure --build DIR --output FILE` and `fixed_lane_screen.py --mode fixed` freeze shader controls; preparation output must be `DIR/qualification.json`. `backing_reuse_probe.py --phase prepare|measure --build DIR --output FILE` similarly requires a frozen qualification. Use fresh output/build paths, preserve manifests, and close every task capture window before timing. The existing aligned production frozen bundle is a prerequisite.','',
'Raw `gpu-aligned-host-profile`, `gpu-lane-*`, `gpu-backing-reference-*` and `gpu-backing-reuse-*` JSON/CSV files retain samples, qualifications, command/source/artifact hashes and external-load observations. Large binaries/traces stay ignored under `benchmarks/.build`. Run `python3 native/benchmarks/fair/host_lane_report.py` to validate the local frozen artifacts and regenerate this report. No production tests were rerun because this pass changes diagnostic tooling/evidence only.']
validity=dict(initial_series=[dict(path=f'gpu-lane-{m}-paired-2026-10-29.json',sha256=p.sha(R/f'gpu-lane-{m}-paired-2026-10-29.json'),acceptance=False,disposition='Potentially contaminated by retained Xcode GPU debugging session; superseded by repeats.') for m in repeats],correction='A hidden A-sharing GPU debugging session was found after the initial series. It was stopped and all task capture windows closed before repeats and the backing-reuse screen. Original load-note assertions are superseded. No initial timing data were deleted.',repeat_series=[dict(path=f'gpu-lane-{m}-repeat-2026-10-29.json',sha256=p.sha(R/f'gpu-lane-{m}-repeat-2026-10-29.json'),blocks=load(f'gpu-lane-{m}-repeat-2026-10-29.json')['protocol']['blocks'],acceptance=False,disposition='Isolated rejection screen after closing captures; not production acceptance.') for m in repeats])
(R/'gpu-lane-validity-2026-10-29.json').write_text(json.dumps(validity,indent=2)+'\n')
(R/'gpu-host-lane-reuse-2026-10-29.md').write_text('\n'.join(lines)+'\n')
print('Audited host profile, shader controls, repeats and backing-reuse evidence.')
