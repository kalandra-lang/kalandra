"""Audit diagnostic controls; these are not production acceptance results."""
import hashlib,json,statistics as st
from pathlib import Path
root=Path(__file__).resolve().parents[3];out=root/'benchmarks/results'
file=out/'gpu-command-lower-bound-2026-10-29.json';data=json.loads(file.read_text())
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
for collection in ('source_sha256','artifact_sha256'):
 for path,digest in data[collection].items():assert sha(root/path)==digest,path
assert not data['production_candidate'] and not data['cancellation_qualification']
assert len(data['verification'])==117 and all(r['bit_identical'] for r in data['verification'])
shapes=((1,64),(1,512),(16,512),(64,512));backends=('bounded','indivisible','blocking','mps')
values={}
for m,d in shapes:
 for mode in range(3):
  dispatches=set()
  for backend in backends:
   rows=[r for r in data['observations'] if r['cell']==[backend,m,d,mode]];assert len(rows)==5
   def median(key):return st.median(st.median(s.get(key,0)/r['iterations'] for s in r['samples'] if s['round']>=0) for r in rows)
   values[backend,m,d,mode]={key:median(key) for key in ('ns','gpu_ns','encoding_ns','submissions','dispatches')}
   if backend!='mps':
    dispatches|={s['dispatches']/r['iterations'] for r in rows for s in r['samples']}
    commands=(5 if m==64 else 3 if m==16 else 1) if backend=='bounded' else 1
    assert all(s['submissions']==commands*r['iterations'] for r in rows for s in r['samples'])
  assert len(dispatches)==1 and dispatches=={11 if m==1 else 12}
lines=['# GPU command and caller lower bounds','','**Diagnostic experiments, not production optimizations.** The controls remove the prepared-region work checkpoints; the blocking variant additionally waits on its task worker. Applying either to arbitrary cancellable source tasks would change their guarantees. All shaders, operation cost/fusion checks, inputs and numerical trees remain unchanged.','',
'For 64×512 resident execution, instrumented medians are 1.452 ms bounded, 0.847 ms whole-block, 0.840 ms caller-blocking and 0.537 ms MPSGraph. The whole-block reduction is about 42%; blocking adds little here. Reported GPU intervals are 0.510, 0.478, 0.481 and 0.255 ms respectively. Removing handoffs does not remove the remaining device-compute/dispatch gap.','',
'The result warrants a distinct closed synchronous entry with an opaque private scheduler. It must expose no external cancellation handle and dynamically prove that no independent runnable/external operation can progress. Ordinary TaskCore calls retain bounded cancellation. This proof and implementation are future work, not properties of these controls.','',
'## Measurements','','Five shuffled fresh-process rounds, two warmup and three measured batches per process, adaptive 30 ms target. Profiling is enabled on the Kalandra controls; these wall times are diagnostic and must not replace uninstrumented acceptance. Encoding and GPU intervals are inclusive, overlapping counters, not additive exclusive wall-time buckets. Background load is retained separately.','','| Shape | Mode | Control | Wall µs | GPU interval µs | Recorded encoding µs | Commands |','|---|---|---|---:|---:|---:|---:|']
for m,d in shapes:
 for mode,label in enumerate(('resident','activation','all uploads')):
  for backend in backends:
   v=values[backend,m,d,mode]
   lines.append(f'| {m} × {d} | {label} | {backend} | {v["ns"]/1000:.2f} | {v["gpu_ns"]/1000:.2f} | '+(f'{v["encoding_ns"]/1000:.2f} | {v["submissions"]:.0f} |' if backend!='mps' else '— | — |'))
lines+=['','## Verification and provenance','','117 full-output comparisons across 13 regular/odd shapes and all upload modes pass bit-for-bit against the frozen runtime with Metal API/shader validation. The frozen outputs already passed the independent CPU/MPSGraph numerical oracle. Runtime resource cleanup assertions pass on every call. Every variant uses the same 11 dispatches for one-row blocks and 12 otherwise; only prepared-command boundaries change.','',
'The initial probe changed the unrelated generic-graph limit. Its submission-count assertion rejected it during calibration despite correct outputs. That failure is retained. The corrected probe changes only the prepared-progression checkpoint threshold, leaving cost saturation and fusion admission unchanged. No production source is edited.','',
'Reproduce with `native/benchmarks/command_lower_bound.py --evidence benchmarks/results/gpu-encoding-overlap-paired-2026-10-29.json --build <new-directory> --output <new-json>`, using the frozen artifacts recorded in the source. No cancellation/lifetime qualification is claimed for these benchmark-only controls.','','- [Raw diagnostics](gpu-command-lower-bound-2026-10-29.json)','- [Load](gpu-command-lower-bound-load-2026-10-29.json)','- [Rejected initial probe](gpu-command-lower-bound-initial-failure-2026-10-29.json)','- [Initial load](gpu-command-lower-bound-initial-load-2026-10-29.json)']
base=out/'gpu-command-lower-bound-2026-10-29';base.with_suffix('.md').write_text('\n'.join(lines)+'\n')
import matplotlib;matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':12,'text.color':'#eeeeF4','axes.labelcolor':'#bbb6c9','xtick.color':'#bbb6c9','ytick.color':'#eeeeF4'})
fig,ax=plt.subplots(figsize=(13,8),facecolor='#0d0c12');ax.set_facecolor('#0d0c12');y=np.arange(4)
for i,(backend,label,color) in enumerate(zip(backends,('Bounded task','Whole block (experimental)','Blocking (experimental)','MPSGraph'),('#ad92ff','#82a4bd','#828694','#e7aa82'))):
 vs=[values[backend,m,d,0]['ns']/1000 for m,d in shapes];bars=ax.barh(y+(i-1.5)*.18,vs,height=.16,color=color,label=label)
 ax.bar_label(bars,labels=[f'{v:.0f}' for v in vs],padding=5,fontsize=10)
ax.set_yticks(y,[f'{m} × {d}' for m,d in shapes]);ax.invert_yaxis();ax.set_xlabel('Resident wall time (µs) · lower is faster',labelpad=18)
for spine in ax.spines.values():spine.set_visible(False)
ax.grid(axis='x',alpha=.15);ax.set_axisbelow(True);ax.set_xlim(0,1650);ax.legend(ncol=2,loc='lower left',bbox_to_anchor=(0,1.02),frameon=False,fontsize=11)
fig.text(.08,.95,'GPU handoff costs: diagnostic lower bounds',fontsize=25,weight='bold');fig.text(.08,.90,'Identical outputs and shaders · experimental controls change cancellation',color='#bbb6c9')
fig.text(.08,.07,'Instrumented diagnostic runs, not production acceptance · Apple M2 Max · 29 Oct 2026',fontsize=10,color='#bbb6c9');fig.text(.08,.04,'Whole-block and blocking variants remain outside production.',fontsize=10,color='#bbb6c9')
fig.subplots_adjust(left=.14,right=.94,top=.73,bottom=.18);fig.savefig(base.with_suffix('.png'),dpi=160);fig.savefig(base.with_suffix('.svg'));plt.close(fig)
print(base.with_suffix('.md'))
