"""Audit and render whole-block contiguous-transpose acceptance evidence."""
import json,hashlib,statistics as st
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];OUT=ROOT/'benchmarks/results'
def load(name):return json.loads((OUT/name).read_text())
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
files={s:f'gpu-transpose-view-{s}-2026-10-29.json' for s in ('paired','control','repeat','streaming','matched','qualification','load','streaming-verification','confirm-paired','confirm-control','confirmation-load')}
data={s:load(f) for s,f in files.items()}
for key in ('paired','control','repeat','streaming','matched','confirm-paired','confirm-control'):
 d=data[key]
 for f,h in d['artifact_sha256'].items():assert sha(ROOT/f)==h,f
 for f,h in d['source_sha256'].items():assert sha(ROOT/f)==h,f
 assert {r['mode'] for r in d['qualification']['records']}=={'optimized','debug','sanitized','thread-sanitized'}
 assert all(r['bit_identical_to_baseline'] for r in d['verification'] if r['backend'] in ('old','new','control'))
 for r in d['observations']:
  backend,m,w,mode=r['cell']
  if backend in ('cpu','mps'):continue
  commands=4 if key=='streaming' else 5 if (m,w)==(64,512) else 3 if (m,w) in ((16,512),(64,256)) else 1
  assert all(s['submissions']==commands*r['iterations'] for s in r['samples'])
  assert all(s['gpu_ns']==s['encoding_ns']==s['dispatches']==0 for s in r['samples'])
v=data['streaming-verification'];assert len(v['checks'])==78 and all(r['bit_identical'] for r in v['checks'])
assert v['paired_evidence_sha256']==sha(OUT/files['paired'])
for f,h in v['artifact_sha256'].items():assert sha(ROOT/f)==h,f
q=data['qualification'];assert len(q['tests'])==26 and all(t['passed'] for t in q['tests'])
for f,h in q['sha256'].items():assert sha(ROOT/f)==h,f
for key in ('load','confirmation-load'):
 assert data[key]['samples'] and max(r['external_total_cpu_percent'] for r in data[key]['samples'])<=500

def pairs(d):
 groups={}
 for row in d['pairs']:groups.setdefault(tuple(row['group']),[]).append(row['time_reduction']*100)
 return groups
primary=pairs(data['paired']);shapes=sorted({k[:2] for k in primary},key=lambda s:(s[1],s[0]))
lines=['# Contiguous GPU transpose views','',
'Compared with frozen `aacae818` on Apple M2 Max, 29 October 2026. A singleton or empty transpose shares immutable backing with new shape metadata. General matrix kernels and arithmetic are unchanged.','',
'The optimization applies to the one-row shapes in this study. Other shapes are retained as regression controls; small timing differences there are not evidence of a transpose-view speedup. Pure transpose chains additionally verify zero submissions and unchanged physical bytes.','',
'One-row resident medians improve 0.37–3.07% in the primary run and 0.98–4.01% in replication; the additional frozen confirmation ranges from 0.01% to 2.53%. Treat these as modest improvements, especially the nearly flat 1×256 confirmation. Streaming medians improve 0.77–2.99%. The fresh largest resident block takes 1,560.30 µs: 7.04× faster than the own CPU path, but 2.85× MPSGraph time.','',
'The 64×256 primary resident regression of 2.42% does not repeat against the frozen binary (+0.37%), while its same-binary confirmation moves −2.33%. The 64×64 all-upload controls remain negative (−1.01% to −5.27%). These shapes cannot use a view, so the evidence does not establish a mechanism or a blanket performance gain; every result is retained. Large positive control excursions (+21.97% on an inactive 64×512 case and +19.04% on 1×256 activation upload) are also retained and are not presented as expected gains.','',
'## Paired results','',
'Positive values mean less elapsed time. Five fresh-process ABBA/BAAB blocks for primary/repeat, three for initial policy/streaming controls and five for both confirmation studies; two warmups and three measured batches per process. Adaptive batches target at least 30 ms. Timings include complete invocation, selected owned uploads and cleanup, excluding readback and cold preparation.','']
for study,title in [('paired','Frozen baseline'),('control','Same-binary policy control'),('repeat','Independent replication'),('streaming','Streaming fallback control'),('confirm-paired','Frozen-baseline confirmation'),('confirm-control','Same-binary confirmation')]:
 lines += [f'### {title}','','| Tokens × width | Resident | Activation upload | All uploads |','|---|---:|---:|---:|']
 g=pairs(data[study])
 for m,d in sorted({k[:2] for k in g},key=lambda s:(s[1],s[0])):
  cells=[]
  for mode in range(3):
   xs=g[(m,d,mode)];cells.append(f'{st.median(xs):+.2f}% ({sum(x>0 for x in xs)}/{len(xs)} positive)')
  lines.append(f'| {m} × {d} | '+' | '.join(cells)+' |')
 lines.append('')
lines+=['The policy control toggles only `contiguousTranspose` in the same binary. The streaming control also disables pre-encoding in both members. Removing a logical stage can shift fusion alignment in streaming: the tested one-row block retains 12 dispatches there, versus 11 in the normal work-bounded path. Command counts match between both members of every timed comparison.','','## Fresh CPU and MPSGraph comparison','','Five shuffled process rounds. Use paired controls to attribute changes; absolute timings drift between studies. This is one unmasked, single-head, bias-free pre-RMSNorm transformer block with a 4× ReLU FFN, not full LLM generation. MPSGraph is a benchmark reference only.','','| Tokens × width | Mode | CPU µs | Metal µs | MPSGraph µs | Metal / MPS time |','|---|---|---:|---:|---:|---:|']
matched={tuple(r['cell']):r['median_ns']/1000 for r in data['matched']['summary']}
for m,d in shapes:
 for mode,name in enumerate(('resident','activation','all uploads')):
  cpu=matched['cpu',m,d,0];gpu=matched['new',m,d,mode];mps=matched['mps',m,d,mode]
  lines.append(f'| {m} × {d} | {name} | {cpu:.2f} | {gpu:.2f} | {mps:.2f} | {gpu/mps:.2f}× |')
lines+=['','## Correctness and limits','',
'All output cells match the frozen runtime across 13 regular/odd shapes and all residency modes. A separate 78-comparison check verifies both streaming policies against those same qualified outputs. Independent CPU/MPSGraph numerical bounds pass. Shader text, Metal libraries and CPU LLVM remain unchanged. Profiling and Metal validation are disabled during timing, and no tests or builds overlap acceptance. External CPU load is sampled and retained.','',
'Four prepared-program modes cover exact bit payloads, nine-stage all-view programs, singleton/empty/general matrices, live aliases across differently shaped descriptors, original-input immutability, changing captures, eight concurrent calls, every injected allocation failure, every held cancellation/deadline/device-failure boundary, and teardown. Twenty-six additional runtime/numerical test groups pass, including transform coverage for all ten types.','',
'The new streaming fixture initially failed its old expectation of four live parameter buffers at every boundary. Removing one logical transpose makes the final window contain three. The assertion now checks that exact count; numerical and cancellation assertions were not relaxed. Failed and successful logs are retained in qualification evidence.','',
'No GPU copy or completion exists for an all-view path. Metadata allocation remains fallible; prior submitted work must still acknowledge before result publication. External values retain their non-reusable origin, and live aliases sharing the same physical allocation block destructive pointwise reuse. See the [ownership contract](../../docs/design/gpu-contiguous-transpose.md).','',
'The [general transposed matrix profiles](gpu-transpose-matmul-profiles-2026-10-29.md) remain rejected because of larger-workload regressions. None of those kernels is included in this runtime change.','',
'## Evidence and reproduction','',
'Run `native/benchmarks/transpose_view_acceptance.py` with the frozen `gpu-host-costs-2026-10-29/acceptance` baseline, the `gpu-attention-2026-10-29/acceptance` shader/CPU artifacts, and current `gpu-transpose-view-2026-10-29/prepared` qualification. Its five studies are `paired`, `control`, `repeat`, `streaming`, and `matched`; every source/artifact digest is checked before timing.','']
for f in files.values():lines.append(f'- [{f}]({f})')
base=OUT/'gpu-transpose-view-2026-10-29';base.with_suffix('.md').write_text('\n'.join(lines)+'\n')
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
ax.set_yticks(y,[f'{m} × {d}'+('  • view' if m==1 else '') for m,d in shapes]);ax.invert_yaxis();ax.axvline(0,color='#77717e',lw=1)
lo=min(st.median(v) for v in primary.values());hi=max(st.median(v) for v in primary.values());ax.set_xlim(min(-1,lo-1.5),max(5,hi+2))
ax.set_xlabel('Reduction in complete execution time (%) · positive is faster',labelpad=18)
for spine in ax.spines.values():spine.set_visible(False)
ax.grid(axis='x',alpha=.15);ax.set_axisbelow(True);ax.legend(ncol=3,loc='lower left',bbox_to_anchor=(0,1.02),frameon=False)
fig.text(.08,.95,'Skip copies for contiguous transposes',fontsize=25,weight='bold')
fig.text(.08,.91,'Full transformer block · versus aacae818 · only one-row cases use the view',color='#b4afc4',fontsize=12)
fig.text(.08,.075,'Five ABBA/BAAB blocks per case · Apple M2 Max · 29 Oct 2026',color='#b4afc4',fontsize=10)
fig.text(.08,.047,'Complete invocation, selected uploads and cleanup; all outcomes retained.',color='#b4afc4',fontsize=10)
fig.subplots_adjust(left=.2,right=.94,top=.79,bottom=.17)
fig.savefig(base.with_suffix('.png'),dpi=160);fig.savefig(base.with_suffix('.svg'));plt.close(fig)
audit=dict(evidence_sha256={f:sha(OUT/f) for f in files.values()},report_source_sha256=sha(Path(__file__)),primary_median_reductions={str(k):st.median(v) for k,v in primary.items()})
base.with_suffix('.json').write_text(json.dumps(audit,indent=2)+'\n');print(base.with_suffix('.md'))
