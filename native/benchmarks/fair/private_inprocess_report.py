"""Audit same-runtime GPU/host paired intervals, controls and replay cleanup."""
import json,statistics as st
from pathlib import Path
import protocol as p
R=p.ROOT/'benchmarks/results';B=p.ROOT/'benchmarks/.build/gpu-private-inprocess-v2-2026-10-29'
def load(path):return json.loads(path.read_text())
def obs_group(data):return data['observations'][0]['group']
def identity(data):
 for field in ('signature','artifacts','source_sha256','artifact_sha256'):
  for name,digest in data.get(field,{}).items():
   candidates=[p.ROOT/name,p.ROOT/'benchmarks/.build/gpu-private-output-probe-2026-10-29/frozen-sources'/name,p.ROOT/'benchmarks/.build/gpu-private-inprocess-v2-2026-10-29/frozen-sources'/name]
   assert any(x.is_file() and p.sha(x)==digest for x in candidates),name
q=load(B/'qualification.json');identity(q);identity(load(B/'manifest.json'))
assert len(q['checks'])==144 and sum(x['comparisons'] for x in q['checks'])==576 and all(x['passed'] for x in q['checks'])
assert len(q['oracles'])==16 and all(x['metrics']['max_error_bound_ratio']<=1 for x in q['oracles'])
lines=['# Same-runtime GPU allocation investigation — 29 October 2026','','This diagnostic keeps one runtime, queue, prepared program and resident inputs alive across paired ABBA/BAAB intervals. Production remains unchanged. Cache switching occurs only at quiescent boundaries; idle storage is trimmed and two complete 1,024-call warm-up intervals precede every measured interval. Both sides enable GPU/encoding counters. Each case uses five blocks and two initial warm-up slots. Setup, trimming and warm-up are outside measured intervals; all per-invocation work is inside. These instrumented observations are not uninstrumented production acceptance.','','576 full-output comparisons pass across eight shapes, both switching orders, all runner policies/upload modes, with opposite-pattern warm buffers before each observed result. Sixteen independent high-precision oracle checks pass; the float32 decoder preserves signed zero. This checks numerical behavior of the diagnostic, not full production ownership/fault qualification.','','The precision goal declared before the first final AA output was absolute median wall-time drift ≤2% and every block within ±5%. A failed control cannot establish a roughly 10% total-time benefit. No samples are removed. GPU timestamps measure complete command intervals, including stalls or scheduling, not exclusive arithmetic time. Encoding scopes are inclusive and must not be added to GPU time. Wall minus summed GPU duration is elapsed time outside those timestamp intervals, not CPU consumption.','','| Environment stage | Study | Rows / calls per slot | Wall reduction | Wall block range | GPU interval reduction | Encoding reduction | AA precision goal |','|---|---|---|---:|---:|---:|---:|---|']
all_data=[]
for prefix,label in [('', 'Before cleanup'),('clean-','After graceful signal (partial cleanup)'),('verified-clean-','After verified replay termination'),('large-','Mislaunched64-row shorter intervals'),('large-validated-','Validated512-row larger workload'),('large-long-','Longer512-row intervals')]:
 for study in ('aa','toggle','repeat'):
  stem=f'gpu-private-inprocess-{prefix}{study}-2026-10-29'
  data=load(R/(stem+'.json'));identity(data);all_data.append(data)
  assert data['qualification_sha256']==p.sha(B/'qualification.json')
  count=256 if prefix in ('large-','large-validated-') else 1024
  m=512 if prefix in ('large-validated-','large-long-') else 64
  assert obs_group(data)==[m,512,0]
  assert data['protocol']==dict(blocks=5,iterations=count,per_slot_warmup_intervals=2,initial_warmups=2,order='ABBA/BAAB',profile=True)
  assert len(data['observations'])==1 and len(data['pairs'])==5
  obs=data['observations'][0];rows=obs['samples'];timed=[r for r in rows if 'ns' in r];cache=[r for r in rows if r.get('cache')];states=[r for r in rows if r.get('pair_state')]
  assert len(timed)==len(cache)==len(states)==22 and [r['round'] for r in timed]==list(range(-2,20))
  for sample,c,state in zip(timed,cache,states):
   assert sample['round']==c['round']==state['round']
   assert sample['checksum']==m*512*4*count and sample['gpu_ns']>0 and sample['encoding_ns']>0
   assert sample['submissions']>=count and sample['submissions']%count==0
   assert c['retained_bytes']==c['live_bytes']+c['cached_bytes'] and c['cached_bytes']<=16*1024*1024 and c['peak_retained_bytes']<=64*1024*1024
   assert bool(c['cached_bytes'])==state['enabled']
   assert state['enabled']==(state['current'] and study!='aa')
  for pair in data['pairs']:
   selected=[(r,s) for r,s in zip(timed,states) if r['round']>=0 and r['round']//4==pair['block']]
   before=[r for r,s in selected if not s['current']];after=[r for r,s in selected if s['current']]
   assert len(before)==len(after)==2
   for field in ('ns','gpu_ns','encoding_ns'):
    old=st.mean(x[field]/count for x in before);new=st.mean(x[field]/count for x in after);metric=pair['metrics'][field]
    assert abs(metric['baseline']-old)<.001 and abs(metric['current']-new)<.001 and abs(metric['reduction']-(1-new/old))<1e-12
  metrics={field:[r['metrics'][field]['reduction']*100 for r in data['pairs']] for field in ('ns','gpu_ns','encoding_ns')};v=metrics['ns']
  passes=abs(st.median(v))<=2 and max(map(abs,v))<=5
  lines.append(f'| {label} | {study} | {m} / {count} | {st.median(v):+.2f}% | {min(v):+.2f}% … {max(v):+.2f}% | {st.median(metrics["gpu_ns"]):+.2f}% | {st.median(metrics["encoding_ns"]):+.2f}% | {("pass" if passes else "fail") if study=="aa" else "—"} |')
  load_data=load(R/(stem+'-load.json'));assert load_data['samples']
large=load(B/'large-qualification.json');identity(large)
assert len(large['checks'])==18 and sum(x['comparisons'] for x in large['checks'])==72 and all(x['passed'] for x in large['checks'])
assert len(large['oracles'])==2 and all(x['metrics']['max_error_bound_ratio']<=1 for x in large['oracles'])
telemetry=load(R/'gpu-private-inprocess-dvfs-2026-10-29.json');identity(telemetry)
intervals={r['round']:r for r in telemetry['raw_intervals'] if r.get('interval')}
for row in telemetry['joined']:
 interval=intervals[row['round']];start,end=interval['start_ns'],interval['end_ns']
 weights=[(max(0,min(end,x['end_ns'])-max(start,x['start_ns'])),x) for x in telemetry['raw_samples']]
 total=sum(w for w,x in weights)
 assert abs(row['coverage']-total/(end-start))<1e-12
 for key in ('gpu_freq_mhz','gpu_active_ratio','gpu_power_w'):
  assert abs(row[key]-sum(w*x[key] for w,x in weights)/total)<1e-9
assert len(telemetry['joined'])==20 and min(x['coverage'] for x in telemetry['joined'])>.99
correlation=st.correlation([x['gpu_ns'] for x in telemetry['joined']],[1/x['gpu_freq_mhz'] for x in telemetry['joined']])
assert .91<correlation<.93
cleanup=load(R/'gpu-private-inprocess-replay-cleanup-2026-10-29.json')
lines+=['','## Environment investigation','','The first identical-build control already fails in a single long-lived process. Its GPU command intervals vary widely while encoding is much steadier, so independent-process startup is not a sufficient explanation. Read-only GPU telemetry after that series reports no recorded GPU recovery; a post-run idle utilization snapshot cannot establish idleness during measurements.','','Sixteen live replay services were identified from open trace paths belonging exclusively to this task. Graceful SIGTERM did not stop all of them. The clean-named files therefore describe partial cleanup; they are not evidence that every service had exited. A later verified-termination cohort retains the same frozen executable and methodology. Saved cleanup records distinguish requested signals, trace ownership and verified exits. Xcode, Instruments and unrelated user services are outside that process-cleanup scope. Ordered before/after cohorts cannot alone prove that replay services caused every timing change.','','## Reproduction','','`private_output_inprocess.py --phase prepare --build NEW --output NEW/prepare.json`, followed by `--phase verify --output NEW/qualification.json`, builds and qualifies a copied runtime. For timing use `--phase measure --study aa|toggle|repeat --scope largest --count 1024 --blocks 5 --build NEW --output FRESH.json`. Redirect full output to files; record external load. Never overlap timing with builds, tests or profiling. Frozen artifacts stay local under ignored `benchmarks/.build`; raw observations, hashes, numerical qualification and environment records remain retained. Run this auditor to recompute all paired metrics before interpretation.']
lines+=['','## Frequency and workload follow-up','','A separate diagnostic samples rootless macmon reported DVFS residency while timing the unchanged runtime. Twenty intervals have >99% telemetry coverage. Reported weighted frequency spans about 1,014–1,397 MHz; GPU interval time correlates with inverse frequency (Pearson '+f'{correlation:.3f}'+'). This is a strong association, not proof of sole cause or an instantaneous hardware clock. Sampling deliberately overlaps this profiling cohort only, never acceptance. The temporary MIT helper is isolated from production; its upstream revision and source/license hashes are recorded.','','The first large-named cohort was accidentally launched with the 64-row base tool. Its metadata records the true shape; those samples remain retained and are excluded from larger-workload conclusions. The corrected 512×512 workload adds 72 full-output comparisons and two oracles, keeping shaders/runtime unchanged. With 1,024 calls per slot the identical-build control finally meets the original precision goal: -0.22% median, every block within ±2.89%. Enabled reuse reduces elapsed time 13.01%, independently repeated at 14.18%; every enabled block improves. These are prototype results. Production promotion requires its own ownership/fault/cancellation qualification and fresh measurements.','','Replay cleanup did not make the 64-row control pass, so no small-workload total-time claim follows. Every unfavorable and mislaunched cohort remains in the table.']
(R/'gpu-private-inprocess-2026-10-29.md').write_text('\n'.join(lines)+'\n')
print('Audited 576 full outputs, 16 oracles, and all same-runtime paired/control observations.')
