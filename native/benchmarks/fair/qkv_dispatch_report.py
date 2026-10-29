"""Audit the isolated three-projection controls; retain every paired outcome."""
import hashlib,json,statistics as st
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];OUT=ROOT/'benchmarks/results'
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
evidence={};summaries=[]
for mechanism,controls in [('dispatch',('split','serial')),('concurrent',('serial',))]:
 qfile=ROOT/f'benchmarks/.build/gpu-qkv-{mechanism}-2026-10-29/qualification.json'
 q=json.loads(qfile.read_text());assert len(q['checks'])==81 and all(r['passed'] for r in q['checks'])
 qout=OUT/f'gpu-qkv-{mechanism}-qualification-2026-10-29.json';qout.write_bytes(qfile.read_bytes());evidence[qout.name]=sha(qout)
 for control in controls:
  file=OUT/f'gpu-qkv-{mechanism}-{control}-2026-10-29.json';data=json.loads(file.read_text());evidence[file.name]=sha(file)
  assert data['qualification_sha256']==sha(qfile)
  for item in (q,data):
   for field in ('source_sha256','artifact_sha256'):
    for name,digest in item[field].items():assert sha(ROOT/name)==digest,name
  assert data['protocol']['blocks']==7 and data['control']==control
  grouped={};gpu={}
  for row in data['pairs']:grouped.setdefault(tuple(row['group']),[]).append(row)
  assert len(grouped)==4 and all(len(v)==7 for v in grouped.values())
  for row in data['observations']:
   key=(tuple(row['cell'][1:]),row['process']);version=row['cell'][0]
   values=[sample['gpu_ns']/row['iterations'] for sample in row['samples'] if sample['round']>=0]
   assert all(v>0 for v in values)
   gpu.setdefault(key,{}).setdefault(version,[]).append(st.median(values))
  shapes=[]
  for shape,rows in sorted(grouped.items()):
   gpu_reductions=[]
   for row in rows:
    g=gpu[(shape,row['block'])];assert all(len(g[v])==2 for v in ('baseline','current'))
    gpu_reductions.append(100*(1-st.mean(g['current'])/st.mean(g['baseline'])))
   shapes.append(dict(shape=list(shape),baseline_us=st.median(r['baseline_ns'] for r in rows)/1000,current_us=st.median(r['current_ns'] for r in rows)/1000,reduction_percent=st.median(r['time_reduction'] for r in rows)*100,winning_blocks=sum(r['time_reduction']>0 for r in rows),gpu_reduction_percent=st.median(gpu_reductions),gpu_block_reductions=gpu_reductions,blocks=rows))
  loadfile=OUT/f'gpu-qkv-{mechanism}-{control}-2026-10-29-load.json';load=json.loads(loadfile.read_text());evidence[loadfile.name]=sha(loadfile)
  samples=[r['external_cpu_percent'] for r in load['samples']]
  summaries.append(dict(mechanism=mechanism,control=control,load=dict(samples=len(samples),median=st.median(samples),maximum=max(samples)),shapes=shapes))
resource=OUT/'gpu-resource-probe-2026-10-29.json';evidence[resource.name]=sha(resource)
lines=['# Strict Q/K/V dispatch screen','',
 'Apple M2 Max, 29 October 2026. Isolated kernel evidence only; production is unchanged. This experiment compares a two-dimensional batched dispatch with three ordinary dispatches in a concurrent encoder. Each thread retains the original register16 products and adjacent-pair reduction tree. There is no FMA, reassociation, input packing, or vendor numerical library.', '',
 '## Controls and paired outcomes','',
 'The split control uses three encoders and three dispatches. The serial control uses one encoder and three unchanged dispatches. The dispatch candidate uses one encoder and one dispatch, with separate input-weight and output buffers for Q, K and V. The concurrent candidate uses three unchanged register16 dispatches in one concurrent encoder. Both controls use the same frozen executable and shader library as the candidate. Positive percentages mean less elapsed time; percentages are medians of paired ratios, not ratios of the displayed medians. No sample or outlier is discarded.','',
 '| Candidate / control | M×K×N (each projection) | Control µs | Candidate µs | Time reduction | Winning blocks | GPU time reduction |',
 '|---|---|---:|---:|---:|---:|---:|']
for row in summaries:
 for c in row['shapes']:lines.append(f"| {row['mechanism']} / {row['control']} | {'×'.join(map(str,c['shape']))} | {c['baseline_us']:.3f} | {c['current_us']:.3f} | {c['reduction_percent']:+.2f}% | {c['winning_blocks']}/7 | {c['gpu_reduction_percent']:+.2f}% |")
lines+=['','## Qualification and timing scope','',
 '162 validation case executions passed: two frozen candidates × nine shapes × three input patterns × three execution modes. Every output bit is compared with the original GPU entry. Finite mixed-scale and signed-zero patterns also use a separately rounded CPU adjacent-pair oracle; the IEEE pattern checks compatibility with existing GPU behavior, not independent IEEE conformance. Weights differ between the three projections to expose incorrect buffer selection. Every input and every output guard is checked. Odd tails and non-power-of-two contraction lengths are included. Metal API and shader validation were enabled during qualification.', '',
 'Seven ABBA/BAAB blocks per shape use fresh processes, two warmups and three measured batches. Batches target 30 ms, capped at 256 three-projection invocations; both sides use the same count calibrated from the faster side. Every warmup and measured batch checks all output bits, input immutability and guards outside timing. Timings include command encoding/submission/wait with preallocated resident buffers. They exclude CPU oracle setup, input upload, allocation and readback. This is not whole-transformer or language-level acceptance.', '',
 'Builds, qualification, profiling and screening timings were sequential. Ordinary background applications remained open. Sampled external CPU activity (100% represents one core):','']
for row in summaries:
 l=row['load'];lines.append(f"- {row['mechanism']} / {row['control']}: {l['samples']} samples, median {l['median']:.1f}%, maximum {l['maximum']:.1f}%.")
lines+=['','## Profiling limit and next gate','',
 'Before this screen, a verified baseline FFN contraction was captured with Apple’s [GPU command-line debugger](https://developer.apple.com/documentation/xcode/debugging-with-interactive-command-line-tools). Pipeline identity was confirmed, but `profile run` explicitly reported that GPU profiling requires Apple M3 / A17 Pro or later. The capture is diagnostic, not timing evidence. No register count, spill count or occupancy measurement was obtained. Instruments may expose other counters; the installed CLI recording-options listing did not expose those settings. This does not prove all M2 profiling interfaces lack occupancy support.', '',
 'The subsequent [copied-runtime whole-transformer gate](gpu-independent-dispatch-2026-10-29.md) does not qualify concurrent scheduling: it retains large regressions and noisy inactive controls. The batched one-grid shader remains a separate candidate, not a production speedup. Graph matching must prove three independent compatible projections, preserve each logical result and resource accounting, retain fallback for incompatible shapes/libraries, and respect ordinary cancellation boundaries. A closed-runner-only candidate must remain conditional on its existing no-independent-progress proof. Numerical qualification here does not validate those runtime obligations.', '',
 '## Evidence','']
for name in evidence:lines.append(f'- [{name}]({name})')
base=OUT/'gpu-qkv-dispatch-2026-10-29';base.with_suffix('.md').write_text('\n'.join(lines)+'\n');base.with_suffix('.json').write_text(json.dumps(dict(production_change=False,qualification_cases=162,controls=summaries,evidence_sha256=evidence),indent=2)+'\n')
print(base.with_suffix('.md'))
