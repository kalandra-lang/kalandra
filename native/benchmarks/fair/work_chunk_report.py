"""Audit all work-bounded progression outcomes and render the full-block comparison."""
import collections, hashlib, json, statistics
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parents[3]; out=root/'benchmarks/results'
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
names=[f'transformer-work-chunk-{kind}-2026-10-29.json' for kind in ('paired','control','matched','repeat','qualification','screen','profile','contended','load','load-interrupted','profile-paired')]
paired,control,matched,repeat,qualification,screen,profile,contended,load,interrupted,profile_pair=[json.loads((out/n).read_text()) for n in names]
for data in (paired,control,matched,repeat,screen,profile,contended,profile_pair):
    for name,digest in data['artifact_sha256'].items():
        file=Path(name); assert sha(file if file.is_absolute() else root/file)==digest,name
for name,digest in paired['source_sha256'].items():
    file=Path(name); assert sha(file if file.is_absolute() else root/file)==digest,name
for name,digest in qualification['sha256'].items(): assert sha(root/name)==digest,name
assert all(r['status']=='passed' for r in qualification['tests'])
assert 'user confirmed' in load['context'].lower()
assert max(r['external_total_cpu_percent'] for r in load['samples']) <= 500
shapes=[(m,d) for d in (64,256,512) for m in (1,16,64)]
def commands(m,d,current):
    if current:return 5 if (m,d)==(64,512) else 3 if (m,d) in ((16,512),(64,256)) else 1
    return 6 if (m,d)==(64,512) else 5 if (m,d) in ((16,512),(64,256)) else 4
for row in profile['observations']:
    for sample in row['samples']:
        assert sample['dispatches']==12*row['iterations']
        assert sample['submissions']==commands(row['m'],row['d'],row['current'])*row['iterations']
        assert sample['gpu_ns']>0 and sample['encoding_ns']>0
submission_counts={}
for data in (paired,control,repeat,matched):
    for row in data['observations']:
        version,m,d,mode=row['cell']
        if version in ('cpu','mps'):continue
        current=version in ('current','new'); expected=commands(m,d,current)
        for sample in row['samples']:
            assert sample['submissions']==expected*row['iterations'],(row['cell'],sample)
            # Acceptance deliberately disables diagnostic counters. Verify
            # dispatches separately with profiling, never in these timings.
            assert sample['dispatches']==sample['gpu_ns']==sample['encoding_ns']==0
        submission_counts[f'{version}:{m}x{d}:mode{mode}']=expected

def groups(data):
    result=collections.defaultdict(list)
    for row in data['pairs']:result[tuple(row['group'])].append(row)
    return result
reduction=lambda rows:100*statistics.median(r['time_reduction'] for r in rows)
g,cg,rg=map(groups,(paired,control,repeat))
assert len(g)==len(cg)==27 and len(rg)==12
assert all(len(v)==5 for v in g.values()) and all(len(v)==3 for v in cg.values())
assert all(len(v)==5 for v in rg.values())
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':11,'text.color':'#eeedf2','axes.labelcolor':'#bbb7c9','xtick.color':'#aaa6b9','ytick.color':'#eeedf2','axes.edgecolor':'#292632','savefig.facecolor':'#0d0c12'})
fig,ax=plt.subplots(figsize=(12.8,8.8),facecolor='#0d0c12'); ax.set_facecolor('#0d0c12')
for mode,color,label in [(0,'#ae97ff','Resident inputs'),(1,'#7f9cbf','Activation upload'),(2,'#e6aa82','All inputs uploaded')]:
    values=[reduction(g[m,d,mode]) for m,d in shapes]; ys=[i+(mode-1)*.23 for i in range(9)]
    ax.barh(ys,values,height=.20,color=color,label=label)
    for y,v in zip(ys,values):ax.text(v+(.5 if v>=0 else -.5),y,f'{v:+.1f}%',va='center',ha='left' if v>=0 else 'right',fontsize=9)
ax.set_yticks(range(9),[f'{m} × {d}' for m,d in shapes]); ax.invert_yaxis()
ax.axvline(0,color='#797382',lw=.8); ax.grid(axis='x',color='#292632'); ax.set_axisbelow(True)
ax.set_xlim(min(-3,min(reduction(r) for r in g.values())-5),max(reduction(r) for r in g.values())+7)
ax.set_xlabel('Reduction in complete execution time (%) · positive is faster',labelpad=14)
for s in ('top','right','left'):ax.spines[s].set_visible(False)
ax.legend(loc='upper left',bbox_to_anchor=(0,1.06),ncol=3,frameon=False,fontsize=10)
fig.text(.07,.95,'Less time submitting GPU work',fontsize=24,weight='bold')
fig.text(.07,.908,'Full transformer block · compared with c29e4b14 · every measured case shown',fontsize=12,color='#aaa6b9')
fig.text(.07,.085,'Five ABBA/BAAB blocks · complete invocation and selected uploads · no timed output readback',fontsize=10,color='#aaa6b9')
fig.text(.07,.055,'Same shaders and result bits. Cancellation checkpoints follow estimated work, not four-stage groups.',fontsize=10,color='#aaa6b9')
fig.text(.07,.025,'Apple M2 Max · 29 Oct 2026 · warm pipelines · owned shared upload snapshots for both GPU backends',fontsize=10,color='#aaa6b9')
fig.subplots_adjust(left=.13,right=.95,top=.80,bottom=.20)
base=out/'gpu-work-bounded-progression-2026-10-29'
fig.savefig(base.with_suffix('.png'),dpi=150);fig.savefig(base.with_suffix('.svg'));plt.close(fig)
text=['# Work-bounded GPU progression — 29 October 2026','',
      'Compiler-costed hardware float32 plans use a 64 Mi-unit estimated-work budget instead of also ending every four logical stages. The four parameter slots rotate into the retained owner vector; no floating arithmetic changes. Small transformer graphs use one submission rather than four. The 16×512 and 64×256 cases use three rather than five; 64×512 uses five rather than six. All retain 12 compute dispatches.','',
      'All 27 primary case medians improve: **10.5–55.8% less resident time**, **10.7–54.9% less activation-upload time**, and **3.6–51.1% less all-upload time**. One of 135 primary blocks regresses and remains included. Same-binary control and replication have positive medians and positive individual blocks throughout. The largest resident case is **7.10× faster than Kalandra CPU**, but still takes **2.93× MPSGraph time**; tiny cases still favor CPU.','',
      'Cancellation is still cooperative. Cheap stages may share a checkpoint, so this deliberately changes checkpoint granularity. Cancellation/fault precedence and acknowledgement-before-cleanup remain; neither the old nor new policy is a wall-clock deadline. Oversized individual kernels remain singleton quanta. Unproved plans, software arithmetic, tight budgets and long graphs retain the compatibility path.','',
      '![Full-block execution time](gpu-work-bounded-progression-2026-10-29.png)','',
      '## Frozen baseline comparison','',
      'Five fresh-process ABBA/BAAB blocks against `c29e4b14`, with identical batches within each pair. Positive means less whole-invocation time. Every case and every block is retained. The table gives the median paired time reduction and positive-block count.','',
      '| Tokens × width | Resident | Activation upload | All inputs uploaded |','|---|---:|---:|---:|']
for m,d in shapes:
    text.append(f'| {m} × {d} | '+' | '.join(f'{reduction(g[m,d,mode]):+.2f}% ({sum(r["time_reduction"]>0 for r in g[m,d,mode])}/5 positive)' for mode in range(3))+' |')
text+=['','## Same-binary policy control','','Three blocks per case with one candidate executable; only `workBoundedProgression` changes. This separates batching policy from changes in linking or runtime layout.','','| Tokens × width | Resident | Activation | All uploads |','|---|---:|---:|---:|']
for m,d in shapes:
    text.append(f'| {m} × {d} | '+' | '.join(f'{reduction(cg[m,d,mode]):+.2f}% ({sum(r["time_reduction"]>0 for r in cg[m,d,mode])}/3 positive)' for mode in range(3))+' |')
text+=['','## Independent replication','','Five fresh blocks using the same frozen binaries, including the large control.','','| Tokens × width | Resident | Activation | All uploads |','|---|---:|---:|---:|']
for m,d in [(1,64),(1,512),(16,512),(64,512)]:
    text.append(f'| {m} × {d} | '+' | '.join(f'{reduction(rg[m,d,mode]):+.2f}% ({sum(r["time_reduction"]>0 for r in rg[m,d,mode])}/5 positive)' for mode in range(3))+' |')
summary={tuple(row['cell']):row['median_ns'] for row in matched['summary']}
text+=['','## Fresh CPU and MPSGraph comparison','','Five shuffled rounds of fresh processes. Times are process-median microseconds. GPU upload modes allocate and CPU-copy fresh owned shared buffers for both backends. Timings include invocation and cleanup, but exclude output readback, graph/pipeline preparation and cold compilation. CPU has no upload mode. This is an unmasked, single-head, bias-free pre-RMSNorm transformer block with a 4× ReLU FFN, not LLM generation.','','| Tokens × width | Mode | CPU µs | Kalandra Metal µs | MPSGraph µs | Metal / MPS time |','|---|---|---:|---:|---:|---:|']
for m,d in shapes:
    for mode,label in enumerate(('resident','activation','all uploads')):
        cpu=summary['cpu',m,d,0];gpu=summary['new',m,d,mode];mps=summary['mps',m,d,mode]
        text.append(f'| {m} × {d} | {label} | {cpu/1000:.2f} | {gpu/1000:.2f} | {mps/1000:.2f} | {gpu/mps:.2f}× |')
text+=['','## Separate profiling','','Two fresh diagnostic processes per policy, shape and residency mode confirm all nine shapes retain exactly 12 dispatches and the audited submission counts. Acceptance timing leaves dispatch/device/encoding profiling disabled; those counters are zero there. The first short instrumented batches were too variable for timing inference, including reversed wall-time ordering in the largest case. The table instead uses a separate five-block paired diagnostic with adaptive batches of at least about 30 ms. None of these instrumented times replaces acceptance data. GPU command intervals include dispatch overhead and gaps within a command; changing command boundaries changes that accounting too. Recorded encoding omits some setup and is not an exhaustive CPU breakdown.','','| Tokens × width | Policy | Wall µs | GPU command time µs | Recorded encoding µs | Allocation checkpoints |','|---|---|---:|---:|---:|---:|']
for m,d in ((1,512),(64,512)):
    for current in (False,True):
        rows=[r for r in profile_pair['observations'] if r['cell']==['current' if current else 'baseline',m,d]]
        values=[statistics.median(statistics.median(s[k]/r['iterations'] for s in r['samples'] if s['round']>=0) for r in rows) for k in ('ns','gpu_ns','encoding_ns','attempts')]
        text.append(f'| {m} × {d} | {"work budget" if current else "four stages"} | '+' | '.join(f'{v/(1000 if i<3 else 1):.2f}' for i,v in enumerate(values))+' |')
text+=['','The largest candidate spends roughly **0.51 ms inside GPU command intervals out of 1.56 ms wall time** in this paired diagnostic. This points to substantial remaining setup, submission, acknowledgement and queue-gap costs; it does not isolate any one of them. The next experiment should separately measure command-buffer creation, buffer/argument allocation and inter-command gaps before choosing a reusable-storage or submission-service design. The small and large candidate still encounter 109/113 allocation checkpoints per invocation.','', '## Qualification and limits','',
       '- All baseline/current/control result cells are bit-identical across 13 regular/odd shapes and all three residency modes. CPU and MPSGraph also satisfy the independent propagated numerical bounds. MPSGraph is not asserted bit-identical.',
       '- All 23 shader text artifacts and CPU LLVM are unchanged; timing uses the exact original Metal library binaries. No reassociation, precision change or new vendor dependency.',
       '- Prepared-program safety passes in optimized, debug, ASan/UBSan and TSan modes: changing captures, concurrent calls, allocation failures, teardown and every held cancellation/deadline/device-failure boundary. Small and large costed graphs have 1 and 5 boundaries; streaming retains 4 and 6.',
       '- Higher-rank chains test 5/9/65/513 stages and ordinary, expensive, saturating, invalid and partially missing work metadata. Compiler tests check growing DAG costs and absence of proofs for software arithmetic.',
       '- Broader runtime, upload, storage, compute, numeric, arithmetic-policy, pipeline, ReLU, optimization, graph-batching and tensor-transform tests pass. A stale register-selection assertion from the previous threshold change was corrected, with adjacent threshold shapes added to the independent APFloat test.',
       '- No tests, builds, GPU profiling or plotting overlap acceptance measurements. Process load was sampled every 30 seconds. An earlier full paired run was contaminated by Android Studio Java at 693.1% CPU. A second attempt was interrupted when the user confirmed ongoing work; neither is acceptance evidence. All four accepted studies were restarted after the user explicitly confirmed idle. Their samples, the earlier complete run, and the interrupted log are retained.',
       '- The initial prototype screen is separate evidence, not production qualification: it still used flat map cost estimates. Its script requires the c29e4b14 source checkout.',
       '- MPSGraph remains a benchmark reference only. Small-workload GPU overhead and the largest matrix-kernel gap remain visible in the full table; these results do not establish general parity with vendor libraries.',
       '', '## Evidence and reproduction','']
text += [f'- [{name}]({name})' for name in names]
text += ['- [Interrupted diagnostic log](transformer-work-chunk-interrupted-2026-10-29.log)']
text+=['','Run `native/benchmarks/work_chunk_acceptance.py` with `--acceptance-dir benchmarks/.build/gpu-upload-2026-10-29/acceptance`, `--program-dir benchmarks/.build/gpu-attention-2026-10-29/acceptance`, `--prepared-dir benchmarks/.build/gpu-work-chunks-2026-10-29/prepared`, and a fresh `--build-dir`. Studies are `paired` (5 blocks), `control` (3), `matched` (5), and `repeat` (5). Use a fresh output file for each study. Generated plans come from `PreparedNumericProgramTests.py`. Every raw record includes artifact hashes, calibration, warmup and measured samples, numerical verification and commands. See [the scheduling contract](../../docs/design/gpu-work-bounded-progression.md).','']
base.with_suffix('.md').write_text('\n'.join(text))
base.with_suffix('.json').write_text(json.dumps(dict(evidence={n:sha(out/n) for n in names},artifact_audit='passed',source_audit='passed',submissions_per_invocation=submission_counts,renderer_sha256=sha(Path(__file__))),indent=2)+'\n')
print(base.with_suffix('.md'))
