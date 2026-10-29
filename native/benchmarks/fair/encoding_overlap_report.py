"""Audit and render whole-block encoding-overlap acceptance evidence."""
import json,hashlib,statistics as st
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];OUT=ROOT/'benchmarks/results'
def load(name):return json.loads((OUT/name).read_text())
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
files={s:f'gpu-encoding-overlap-{s}-2026-10-29.json' for s in ('paired','control','repeat','streaming','matched','qualification','load','confirm-paired','confirm-control','confirmation-load')}
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
q=data['qualification'];assert len(q['tests'])==27 and all(t['passed'] for t in q['tests'])
for f,h in q['sha256'].items():assert sha(ROOT/f)==h,f
assert {r['mode'] for r in q['gate']['records']}=={'optimized','debug','sanitized','thread-sanitized'}
for f,h in q['gate']['sha256'].items():assert sha(Path(f))==h,f
for key in ('load','confirmation-load'):
 assert data[key]['samples'] and max(r['external_total_cpu_percent'] for r in data[key]['samples'])<=500

def pairs(d):
 groups={}
 for row in d['pairs']:groups.setdefault(tuple(row['group']),[]).append(row['time_reduction']*100)
 return groups
primary=pairs(data['paired']);chart=pairs(data['confirm-paired']);shapes=sorted({k[:2] for k in primary},key=lambda s:(s[1],s[0]))
lines=['# Overlap encoding with bounded GPU work','',
'Compared with frozen `7ebdeb49` on Apple M2 Max, 29 October 2026. The CPU can encode later commands while the first command runs. One command per invocation remains in flight; shader arithmetic, work limits and acknowledgement/cancellation boundaries are unchanged.','',
'Only multi-command, cost-proven plans with sufficient resource headroom use overlap. Single-command cases use the ordinary-submit fast path instead of the pre-encoded chain; they do not overlap encoding. Streaming remains a policy control. Every outcome, including regressions, is retained. See the [ownership and admission design](../../docs/design/gpu-encoding-overlap.md).','',
'## Outcome','',
'Largest resident time falls 4.17% in the primary study, 3.90% in replication and 3.95% in longer-batch confirmation; same-binary controls give 4.87% and 3.59%. Largest all-upload reductions are 3.46%, 3.76% and 2.13%. Activation-upload confirmation is negative (-1.19%) despite positive earlier/control results. Smaller cases are mixed: longer frozen-baseline resident controls range down to -3.02%, while corresponding same-binary medians are much closer to zero. No blanket improvement or regression-free claim is made. The original -15.31% 16×256 activation result is retained; its longer confirmation is -1.12%.','',
'Fresh largest resident medians are CPU 11.019 ms, Metal 1.516 ms and MPSGraph 0.546 ms: 7.27× our CPU speed and 2.78× MPSGraph time. The handoff/device gap remains substantial.','',
'## Paired results','',
'Positive values mean less elapsed time. Five fresh-process ABBA/BAAB blocks in primary/repeat studies and three in policy/streaming controls. Two warmups and three measured batches per process; adaptive batches target at least 30 ms. Confirmation retains all 27 cases with seven frozen-baseline and five same-binary blocks at a 100 ms target, using exactly the same executables and inputs. Timings include complete invocation, selected owned uploads and cleanup, excluding readback and cold preparation.','']
for study,title in [('paired','Frozen baseline'),('control','Same-binary policy control'),('repeat','Independent replication'),('streaming','Streaming fallback control'),('confirm-paired','Longer-batch frozen-baseline confirmation'),('confirm-control','Longer-batch policy confirmation')]:
 lines += [f'### {title}','','| Tokens × width | Resident | Activation upload | All uploads |','|---|---:|---:|---:|']
 g=pairs(data[study])
 for m,d in sorted({k[:2] for k in g},key=lambda s:(s[1],s[0])):
  cells=[]
  for mode in range(3):
   xs=g[(m,d,mode)];cells.append(f'{st.median(xs):+.2f}% ({sum(x>0 for x in xs)}/{len(xs)} positive)')
  lines.append(f'| {m} × {d} | '+' | '.join(cells)+' |')
 lines.append('')
lines+=['The policy control toggles only `overlappedEncoding` in the same binary. Streaming disables pre-encoding in both members and has a separate full numerical verification sweep; overlap cannot apply there. Submission counts are identical in every timed sample.','','## Fresh CPU and MPSGraph comparison','','Five shuffled process rounds. Use paired controls to attribute changes; absolute timings drift between studies. This is one unmasked, single-head, bias-free pre-RMSNorm transformer block with a 4× ReLU FFN, not full LLM generation. MPSGraph is a benchmark reference only.','','| Tokens × width | Mode | CPU µs | Metal µs | MPSGraph µs | Metal / MPS time |','|---|---|---:|---:|---:|---:|']
matched={tuple(r['cell']):r['median_ns']/1000 for r in data['matched']['summary']}
for m,d in shapes:
 for mode,name in enumerate(('resident','activation','all uploads')):
  cpu=matched['cpu',m,d,0];gpu=matched['new',m,d,mode];mps=matched['mps',m,d,mode]
  lines.append(f'| {m} × {d} | {name} | {cpu:.2f} | {gpu:.2f} | {mps:.2f} | {gpu/mps:.2f}× |')
lines+=['','## Correctness, ownership and resource limits','',
'Every output cell matches the frozen runtime across 13 regular/odd shapes and all residency modes, in both normal and separately verified streaming execution. Independent CPU/MPSGraph numerical bounds pass. Shader text, Metal libraries and CPU LLVM remain unchanged. Profiling and Metal validation are disabled during timing, and no tests or builds overlap acceptance. External CPU load is sampled and retained. Ordinary background applications remain active; this is not a dedicated idle lab machine. Pairing, replication and longer batches reduce drift but do not remove all outliers.','',
'Four prepared-program modes and 27 broader runtime/numerical groups pass. Coverage includes changing captures, concurrent calls, all injected allocation checkpoints, held cancellation/deadline/device-failure boundaries, aliases, ranks, long chains and teardown. The enabled large case has 107 allocation checkpoints versus 99 for the fully pre-encoded control; every new point is injected.','',
'Deterministic gates compile instrumented copies of production code; no gate is present in shipping libraries. They pause the builder and callback independently and require the external lease and resources to remain live until both finish. The original prototype failed the callback-gap regression. The repaired transition handles acknowledgement and the final admission/cleanup decision under one lock, and passes all four runtime modes.','',
'Each overlapping command owns separate parameter backing, avoiding CPU writes to an in-flight shared resource. A 15,732,736-byte budget keeps the original one-arena/five-command policy; 15,794,176 bytes allows five arenas and the same five commands. Charged peak payload is 14,733,312 versus 14,749,696 bytes. Outputs match exactly, both limits hold, and the original 64 KiB safety headroom remains part of admission. No cross-invocation cache or larger work quantum is added.','',
'The production rebuild changed task-archive debug metadata. Stripping debug data produces identical archives, and both timed executables link the exact frozen task archive. Its digests and the equivalence check are recorded. An unused completion parameter was removed after the warning-as-error build caught it.','',
'## Evidence and reproduction','',
'`native/benchmarks/encoding_overlap_acceptance.py` consumes the frozen `gpu-transpose-view-2026-10-29/acceptance` predecessor, `gpu-attention-2026-10-29/acceptance` shaders/CPU reference, and current `gpu-encoding-overlap-2026-10-29/prepared` qualification. Studies are `paired`, `control`, `repeat`, `streaming` and `matched`; input source/artifact digests are checked before timing. Earlier prototype screens must be reproduced from their recorded source commits, not the later production source.','']
for f in files.values():lines.append(f'- [{f}]({f})')
base=OUT/'gpu-encoding-overlap-2026-10-29';base.with_suffix('.md').write_text('\n'.join(lines)+'\n')
import matplotlib;matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':12,'text.color':'#ececf2','axes.labelcolor':'#b4afc4','xtick.color':'#b4afc4','ytick.color':'#ececf2'})
fig,ax=plt.subplots(figsize=(13,9),facecolor='#0d0c12');ax.set_facecolor('#0d0c12')
y=np.arange(len(shapes));colors=['#ae92ff','#829fbe','#e5aa84']
for mode,label in enumerate(('Resident','Activation upload','All uploads')):
 vals=[st.median(chart[(*s,mode)]) for s in shapes]
 bars=ax.barh(y+(mode-1)*.24,vals,height=.21,color=colors[mode],label=label)
 ax.bar_label(bars,labels=[f'{v:+.1f}%' for v in vals],padding=5,fontsize=10)
ax.set_yticks(y,[f'{m} × {d}'+('  · 5 commands' if (m,d)==(64,512) else '  · 3 commands' if (m,d) in ((16,512),(64,256)) else '  · 1 command') for m,d in shapes]);ax.invert_yaxis();ax.axvline(0,color='#77717e',lw=1)
lo=min(st.median(v) for v in chart.values());hi=max(st.median(v) for v in chart.values());ax.set_xlim(min(-1,lo-1.5),max(5,hi+2))
ax.set_xlabel('Reduction in complete execution time (%) · positive is faster',labelpad=18)
for spine in ax.spines.values():spine.set_visible(False)
ax.grid(axis='x',alpha=.15);ax.set_axisbelow(True);ax.legend(ncol=3,loc='lower left',bbox_to_anchor=(0,1.02),frameon=False)
fig.text(.08,.95,'Overlap encoding with bounded GPU work',fontsize=25,weight='bold')
fig.text(.08,.91,'Full transformer block · versus 7ebdeb49 · only multi-command cases overlap',color='#b4afc4',fontsize=12)
fig.text(.08,.075,'Longer-batch confirmation · seven ABBA/BAAB blocks · Apple M2 Max · 29 Oct 2026',color='#b4afc4',fontsize=10)
fig.text(.08,.047,'Complete invocation, selected uploads and cleanup; all outcomes retained.',color='#b4afc4',fontsize=10)
fig.subplots_adjust(left=.27,right=.94,top=.79,bottom=.17)
fig.savefig(base.with_suffix('.png'),dpi=160);fig.savefig(base.with_suffix('.svg'));plt.close(fig)
audit=dict(evidence_sha256={f:sha(OUT/f) for f in files.values()},report_source_sha256=sha(Path(__file__)),primary_median_reductions={str(k):st.median(v) for k,v in primary.items()},chart_study='confirm-paired',chart_median_reductions={str(k):st.median(v) for k,v in chart.items()})
base.with_suffix('.json').write_text(json.dumps(audit,indent=2)+'\n');print(base.with_suffix('.md'))
