"""Audit and render the invocation-private parameter arena study."""
import json,hashlib,statistics as st
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];OUT=ROOT/'benchmarks/results'
def load(name):return json.loads((OUT/name).read_text())
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
files={s:f'gpu-parameter-arena-{s}-2026-10-29.json' for s in ('paired','control','repeat','matched','qualification')}
data={s:load(f) for s,f in files.items()}
for key in ('paired','control','repeat','matched'):
 d=data[key]
 for f,h in d['artifact_sha256'].items():assert sha(ROOT/f)==h,f
 for f,h in d['source_sha256'].items():assert sha(ROOT/f)==h,f
 assert {r['mode'] for r in d['qualification']['records']}=={'optimized','debug','sanitized','thread-sanitized'}
 assert all(r['bit_identical_to_baseline'] for r in d['verification'] if r['backend'] in ('old','new','control'))
 assert all(s['gpu_ns']==s['encoding_ns']==s['dispatches']==0 for r in d['observations'] if r['cell'][0] not in ('cpu','mps') for s in r['samples'])
 for r in d['observations']:
  backend,m,w,mode=r['cell']
  if backend in ('cpu','mps'):continue
  commands=5 if (m,w)==(64,512) else 3 if (m,w) in ((16,512),(64,256)) else 1
  assert all(s['submissions']==commands*r['iterations'] for s in r['samples'])
q=data['qualification'];assert len(q['tests'])==26 and all(t['passed'] for t in q['tests'])
for f,h in q['sha256'].items():assert sha(ROOT/f)==h,f

def pairs(d):
 groups={}
 for row in d['pairs']:groups.setdefault(tuple(row['group']),[]).append(row['time_reduction']*100)
 return groups
primary=pairs(data['paired']);shapes=sorted({k[:2] for k in primary},key=lambda s:(s[1],s[0]))
lines=['# Invocation-private GPU parameter arena','',
'Compared with frozen `fb85c832` on Apple M2 Max, 29 October 2026. Lower elapsed time is better. This change shares argument backing within an invocation; shader arithmetic, work limits and command scheduling stay unchanged.','',
'Primary resident medians improve 3.0–9.6%; 26 of 27 shape/mode medians and 119 of 135 paired blocks improve. The 16×512 all-upload primary median is 0.19% slower and remains included; its control and repeat medians improve 1.25% and 1.92%. Independent resident replication ranges from 0.49% to 10.73%, so the smaller gains should not be treated as precise constants.','',
'## Primary paired study','',
'Five ABBA/BAAB blocks per shape and residency mode, fresh processes, two warmups and three measured batches, adaptive batches of at least 30 ms. Timings include invocation, selected fresh owned uploads and cleanup, excluding readback and cold preparation. All outcomes are retained.','',
'| Tokens × width | Resident | Activation upload | All uploads |','|---|---:|---:|---:|']
for m,d in shapes:
 cells=[]
 for mode in range(3):
  xs=primary[(m,d,mode)];cells.append(f'{st.median(xs):+.2f}% ({sum(x>0 for x in xs)}/{len(xs)} positive)')
 lines.append(f'| {m} × {d} | '+' | '.join(cells)+' |')
for study,title in [('control','Same-binary policy control'),('repeat','Independent replication')]:
 lines+=['',f'## {title}','','Only `parameterArena` changes in the same-binary control; the independent repeat uses fresh processes with the same frozen artifacts.','', '| Tokens × width | Resident | Activation upload | All uploads |','|---|---:|---:|---:|']
 g=pairs(data[study])
 for m,d in sorted({k[:2] for k in g},key=lambda s:(s[1],s[0])):
  vals=[]
  for mode in range(3):
   xs=g[(m,d,mode)];vals.append(f'{st.median(xs):+.2f}% ({sum(x>0 for x in xs)}/{len(xs)} positive)')
  lines.append(f'| {m} × {d} | '+' | '.join(vals)+' |')
lines+=['','## Fresh CPU and MPSGraph comparison','','Process-median microseconds from five shuffled rounds. Absolute medians drift between studies; use the paired and control measurements to attribute the gain from this change. This is a single unmasked, single-head, bias-free pre-RMSNorm block with a 4× ReLU FFN, not full LLM generation. MPSGraph is only a benchmark reference.','', '| Tokens × width | Mode | CPU µs | Metal µs | MPSGraph µs | Metal / MPS time |','|---|---|---:|---:|---:|---:|']
matched={tuple(r['cell']):r['median_ns']/1000 for r in data['matched']['summary']}
for m,d in shapes:
 for mode,name in enumerate(('resident','activation','all uploads')):
  cpu=matched['cpu',m,d,0];gpu=matched['new',m,d,mode];mps=matched['mps',m,d,mode]
  lines.append(f'| {m} × {d} | {name} | {cpu:.2f} | {gpu:.2f} | {mps:.2f} | {gpu/mps:.2f}× |')
lines+=['','## Host costs and rejected alternatives','',
'The preceding baseline diagnostic profile used the [Mach clock shared by Metal GPU timestamps](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime). Inclusive buckets overlap; do not sum them as exclusive time. Median diagnostic microseconds per invocation:','',
'| Shape | Wall | Buffer allocation | Buffer destruction | Command creation | Submit → GPU start | GPU end → callback | GPU intervals |','|---|---:|---:|---:|---:|---:|---:|---:|']
profile=load('gpu-host-costs-profile-2026-10-29.json')
for shape in ((1,64),(1,512),(16,512),(64,512)):
 rows=[r for r in profile['observations'] if (r['m'],r['d'])==shape]
 def median(field,index=None):
  return st.median(st.median((x[field][index] if index is not None else x[field])/r['iterations']/1000 for x in r['samples'] if x.get('host_profile') and x['round']>=0) for r in rows)
 values=[median('wall_ns')]+[median('buckets',i) for i in (0,1,2,6,7,8)]
 lines.append(f'| {shape[0]} × {shape[1]} | '+' | '.join(f'{x:.2f}' for x in values)+' |')
lines+=['','The largest baseline spends approximately 0.49 ms between submission and GPU start, and 0.38 ms between GPU end and callback entry. Allocation is much smaller, at 0.08 ms. These are measured intervals, not proof of a particular driver implementation.','',
'Three benchmark-only replacements used a global default-priority worker, a global interactive worker, or a serial initiated-priority worker, waiting for actual completion before checking cancellation and admitting the next chunk. None provides a reliable win across both shapes; no completion worker change ships. Their per-block time reductions:','', '| Worker | 1 × 512 | 64 × 512 |','|---|---:|---:|']
workers=load('gpu-host-completion-screen-2026-10-29.json')
for name,d in workers['variants'].items():
 g=pairs(d);lines.append(f'| {name} | '+ ' | '.join(', '.join(f'{v:+.2f}%' for v in g[(m,512)]) for m in (1,64))+' |')
lines+=['','## Correctness, ownership and scope','',
'All Kalandra output cells match the frozen baseline across 13 regular/odd shapes and all residency modes. Independent CPU/MPSGraph numerical bounds pass. All 23 shader text artifacts and CPU LLVM remain unchanged, and both GPU variants use the original Metal binaries. Acceptance timing disables profiling. Submission counts are identical in every timing sample.','',
'Four-mode prepared-program qualification covers changing captures, eight concurrent calls, aliases, ranks 1/3/8, 5/9/65/513-stage plans, cost saturation, failed guards, checked casts, teardown, allocation failure and every held cancellation/deadline/device-failure boundary. Parameter backing drops from 16 buffers to one; allocation checkpoints drop 109→95 for the small block and 113→99 for the large block. The separate-buffer control still passes all ownership checks.','',
'A dedicated boundary fixture verifies that padding does not consume the previous 64 KiB admission headroom: 67,584-byte budgets retain separate parameter buffers and the same progression, while 73,728-byte budgets admit the arena. Both return exact matching values, remain within budget and release all bytes. Twenty-six broader storage, arithmetic, pipeline, graph, ReLU, transform and numeric test groups also pass.','',
'The initial prepared test failed because it explicitly expected sixteen live parameter buffers. It now checks one for the arena and sixteen for the control. The failure and successful reruns are retained; no numerical assertion was relaxed.','',
'No global cache, mutable cross-invocation storage, checkpoint relaxation, detached submission thread or vendor numerical dependency is added. The arena uses fresh owned storage with distinct 256-byte-aligned writable slices. See the [storage contract](../../docs/design/gpu-parameter-arena.md).','',
'Next: the dominant remaining gap is command handoff latency. The tested host-worker alternatives do not solve it. Further progress needs a separately qualified submission/checkpoint design; allocation savings alone cannot erase that gap.','',
'## Reproduction and evidence','',
'The diagnostic scripts were run against `fb85c832` before production edits and retain generated source and binary hashes. Run them from that baseline with the recorded frozen work-chunk artifacts. The acceptance runner consumes those same frozen directories plus the current four-mode prepared qualification; it verifies signatures before timing and has `paired`, `control`, `repeat` and `matched` studies.','',
'```sh','python3 native/benchmarks/parameter_arena_acceptance.py \\', '  --acceptance-dir benchmarks/.build/gpu-work-chunks-2026-10-29/acceptance \\', '  --program-dir benchmarks/.build/gpu-attention-2026-10-29/acceptance \\', '  --prepared-dir benchmarks/.build/gpu-host-costs-2026-10-29/prepared \\', '  --build-dir benchmarks/.build/gpu-host-costs-2026-10-29/acceptance \\', '  --study paired --blocks 5 --output /tmp/arena-new-paired.json','```','']
for f in files.values():lines.append(f'- [{f}]({f})')
extra=['gpu-host-costs-profile-2026-10-29.json','gpu-host-completion-screen-2026-10-29.json','gpu-parameter-arena-screen-2026-10-29.json','gpu-parameter-arena-load-2026-10-29.json']
load_data=load('gpu-parameter-arena-load-2026-10-29.json')
assert load_data['samples'] and max(r['external_total_cpu_percent'] for r in load_data['samples'])<=500
import subprocess
baseline_source=hashlib.sha256(subprocess.check_output(['git','show','fb85c832:native/metal/Metal.mm'],cwd=ROOT)).hexdigest()
for name in extra[:3]:
 d=load(name)
 for f,h in d['artifact_sha256'].items():assert sha(ROOT/f)==h,f
 for f,h in d['source_sha256'].items():
  assert (baseline_source if f=='native/metal/Metal.mm' else sha(ROOT/f))==h,f
for f in extra:lines.append(f'- [{f}]({f})')
base=OUT/'gpu-parameter-arena-2026-10-29';base.with_suffix('.md').write_text('\n'.join(lines)+'\n')
import matplotlib;matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':12,'text.color':'#ececf2','axes.labelcolor':'#b4afc4','xtick.color':'#b4afc4','ytick.color':'#ececf2'})
fig,ax=plt.subplots(figsize=(13,9),facecolor='#0d0c12');ax.set_facecolor('#0d0c12')
y=np.arange(len(shapes));colors=['#ae92ff','#829fbe','#e5aa84']
for mode,label in enumerate(('Resident','Activation upload','All uploads')):
 vals=[st.median(primary[(*s,mode)]) for s in shapes]
 bars=ax.barh(y+(mode-1)*.24,vals,height=.21,color=colors[mode],label=label)
 ax.bar_label(bars,labels=[f'{v:+.1f}%' for v in vals],padding=5,fontsize=10)
ax.set_yticks(y,[f'{m} × {d}' for m,d in shapes]);ax.invert_yaxis();ax.axvline(0,color='#77717e',lw=1)
lo=min(st.median(v) for v in primary.values());hi=max(st.median(v) for v in primary.values());ax.set_xlim(min(-1,lo-1.5),max(10,hi+2))
ax.set_xlabel('Reduction in complete execution time (%) · positive is faster',labelpad=18)
for spine in ax.spines.values():spine.set_visible(False)
ax.grid(axis='x',alpha=.15);ax.set_axisbelow(True);ax.legend(ncol=3,loc='lower left',bbox_to_anchor=(0,1.02),frameon=False)
fig.text(.08,.95,'Fewer GPU argument allocations',fontsize=25,weight='bold')
fig.text(.08,.91,'Full transformer block · versus fb85c832 · unchanged shaders and command boundaries',color='#b4afc4',fontsize=12)
fig.text(.08,.075,'Five ABBA/BAAB blocks per case · Apple M2 Max · 29 Oct 2026',color='#b4afc4',fontsize=10)
fig.text(.08,.047,'Complete invocation, selected uploads and cleanup; excludes readback and cold preparation.',color='#b4afc4',fontsize=10)
fig.subplots_adjust(left=.18,right=.94,top=.79,bottom=.17)
fig.savefig(base.with_suffix('.png'),dpi=160);fig.savefig(base.with_suffix('.svg'));plt.close(fig)
audit=dict(evidence_sha256={f:sha(OUT/f) for f in list(files.values())+extra},report_source_sha256=sha(Path(__file__)),primary_median_reductions={str(k):st.median(v) for k,v in primary.items()})
base.with_suffix('.json').write_text(json.dumps(audit,indent=2)+'\n');print(base.with_suffix('.md'))
