"""Audit and render owned shared-upload acceptance, including every regression."""
import collections, hashlib, json, statistics
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root = Path(__file__).resolve().parents[3]
out = root/'benchmarks/results'
sha = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
names = [f'transformer-upload-{kind}-2026-10-29.json' for kind in ('paired','storage','matched','repeat','ack-screen','sync-screen','qualification','cache-control')]
paired,storage,matched,repeat,ack,sync,qualification,cache = [json.loads((out/n).read_text()) for n in names]
for data in (paired,storage,matched,repeat,ack,sync,cache):
    for name,digest in data['artifact_sha256'].items():
        file=Path(name); assert sha(file if file.is_absolute() else root/file)==digest,file
for name,digest in paired['source_sha256'].items():
    file=Path(name); assert sha(file if file.is_absolute() else root/file)==digest,file
for name,digest in qualification['sha256'].items(): assert sha(root/name)==digest,name
submission_counts={}
for row in paired['observations']:
    version,_,m,d,mode=row['cell']
    compute=6 if (m,d)==(64,512) else 5 if (m,d) in ((16,512),(64,256)) else 4
    expected=compute+(0 if version=='current' or not mode else 1 if mode==1 else 7)
    assert all(sample['submissions']==expected*row['iterations'] for sample in row['samples'])
    submission_counts[f'{version}:{m}x{d}:mode{mode}']=expected
def groups(data):
    result=collections.defaultdict(list)
    for row in data['pairs']:result[tuple(row['group'])].append(row)
    return result
reduction=lambda rows:100*statistics.median(r['time_reduction'] for r in rows)
g,sg,rg=map(groups,(paired,storage,repeat))
assert len(g)==27 and all(len(rows)==5 for rows in g.values())
assert len(sg)==54 and all(len(rows)==3 for rows in sg.values())
assert len(rg)==12 and all(len(rows)==5 for rows in rg.values())
shapes=[(m,d) for d in (64,256,512) for m in (1,16,64)]
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':11,'text.color':'#eeedf2','axes.labelcolor':'#bbb7c9','xtick.color':'#aaa6b9','ytick.color':'#eeedf2','axes.edgecolor':'#292632','savefig.facecolor':'#0d0c12'})
fig,ax=plt.subplots(figsize=(12.8,8.8),facecolor='#0d0c12');ax.set_facecolor('#0d0c12')
for mode,color,label in [(0,'#ae97ff','Resident inputs'),(1,'#7f9cbf','Activation upload'),(2,'#e6aa82','All inputs uploaded')]:
    values=[reduction(g['kalandra',m,d,mode]) for m,d in shapes];ys=[i+(mode-1)*.23 for i in range(9)]
    ax.barh(ys,values,height=.20,color=color,label=label)
    for y,v in zip(ys,values):ax.text(v+(.4 if v>=0 else -.4),y,f'{v:+.1f}%',va='center',ha='left' if v>=0 else 'right',fontsize=9)
ax.set_yticks(range(9),[f'{m} × {d}' for m,d in shapes]);ax.invert_yaxis()
ax.axvline(0,color='#797382',lw=.8);ax.grid(axis='x',color='#292632');ax.set_axisbelow(True)
ax.set_xlim(min(-6,min(reduction(r) for r in g.values())-5),max(reduction(r) for r in g.values())+7)
ax.set_xlabel('Reduction in complete execution time (%) · positive is faster',labelpad=14)
for s in ('top','right','left'):ax.spines[s].set_visible(False)
ax.legend(loc='upper left',bbox_to_anchor=(0,1.06),ncol=3,frameon=False,fontsize=10)
fig.text(.07,.95,'Owned uploads on Apple unified memory',fontsize=24,weight='bold')
fig.text(.07,.908,'Full transformer block · compared with dec5a66d · every measured case shown',fontsize=12,color='#aaa6b9')
fig.text(.07,.085,'Five ABBA/BAAB blocks · complete invocation and selected uploads · no timed output readback',fontsize=10,color='#aaa6b9')
fig.text(.07,.055,'Owned snapshots; copies yield every 1 MiB. Numerical, cancellation and lifetime checks passed.',fontsize=10,color='#aaa6b9')
fig.text(.07,.025,'Apple M2 Max · 29 Oct 2026 · warm pipelines · vendor comparisons use matched shared snapshots',fontsize=10,color='#aaa6b9')
fig.subplots_adjust(left=.13,right=.95,top=.80,bottom=.20)
base=out/'gpu-owned-uploads-2026-10-29'
fig.savefig(base.with_suffix('.png'),dpi=150);fig.savefig(base.with_suffix('.svg'));plt.close(fig)

text=['# Owned shared-upload acceptance — 29 October 2026','',
      'The runtime copies CPU inputs into owned shared Metal buffers on unified-memory devices. It removes staging/private blits and their submissions while yielding after each 1 MiB copy chunk. GPU compute kernels, arithmetic, compiler and TaskCore are unchanged. Non-unified devices retain the private-buffer fallback.','',
      'All-upload medians improve **26.4–56.0%**, with every block positive in every case. Activation-upload medians improve **10.5–17.6%**. The independent repeat confirms **25.3–51.6%** all-upload reductions. Resident results are mixed: primary losses reach **2.7%**, while the repeat changes their signs and includes a **2.9%** loss at 1×256. This is an accepted transfer optimization, not a claim of improved resident latency.','',
      '![Full-block upload results](gpu-owned-uploads-2026-10-29.png)','',
      '## Frozen baseline comparison','',
      'Five adjacent ABBA/BAAB blocks against `dec5a66d`; positive is less execution time. The current and baseline use the same generated program and shader bundle. All 27 outcomes are retained. The resident comparison still changes input storage placement even though allocation/upload time is outside its timing.','',
      '| Tokens × width | Resident | Activation upload | All inputs uploaded |',
      '|---|---:|---:|---:|']
for m,d in shapes:
    text.append(f'| {m} × {d} | '+' | '.join(f'{reduction(g["kalandra",m,d,mode]):+.2f}% ({sum(r["time_reduction"]>0 for r in g["kalandra",m,d,mode])}/5 positive)' for mode in range(3))+' |')
text+=['','## Same-binary storage control','','Both Kalandra policies use the exact same candidate executable. Only its internal shared-upload option changes. Both MPS policies also use one executable. Three blocks per group, with all outcomes retained. This separates storage selection from linking/code-layout changes.','','| Tokens × width | Implementation | Resident | Activation | All uploads |','|---|---|---:|---:|---:|']
for m,d in shapes:
    for k in ('kalandra','mps'):
        text.append(f'| {m} × {d} | {k} | '+' | '.join(f'{reduction(sg[k,m,d,mode]):+.2f}%' for mode in range(3))+' |')
text+=['','## Independent replication','','Same frozen acceptance binaries; five fresh blocks for three one-token cases and the largest control.','','| Tokens × width | Resident | Activation | All uploads |','|---|---:|---:|---:|']
for m,d in [(1,64),(1,256),(1,512),(64,512)]:
    text.append(f'| {m} × {d} | '+' | '.join(f'{reduction(rg["kalandra",m,d,mode]):+.2f}%' for mode in range(3))+' |')
summary={tuple(row['cell']):row['median_ns'] for row in matched['summary']}
cg=groups(cache)
assert len(cg)==15 and all(len(rows)==5 for rows in cg.values())
text+=['','## Write-combined cache experiment','','Five paired blocks compare an isolated write-combined upload allocation against the bounded default-cache candidate. Both use the same driver, task archive, program and shaders. The optimized upload lifecycle test passes with validation, and all 15 shape/residency dumps match bit-for-bit. Positive values favor write combining.','','| Tokens × width | Resident | Activation | All uploads |','|---|---:|---:|---:|']
for m,d in [(1,64),(16,64),(1,256),(1,512),(64,512)]:
    text.append(f'| {m} × {d} | '+' | '.join(f'{reduction(cg[m,d,mode]):+.2f}% ({sum(r["time_reduction"]>0 for r in cg[m,d,mode])}/5 positive)' for mode in range(3))+' |')
text+=['','Rejected: write combining loses **20.8% at 1×512 all-upload** and **11.8% at 64×512 all-upload**, with all five blocks negative in each. The mixed resident gains do not justify that tradeoff. Production retains default CPU caching. Apple explicitly notes that [write-combined memory can have performance pitfalls](https://developer.apple.com/documentation/metal/mtlcpucachemode/writecombined); this experiment does not establish the microarchitectural cause of the observed losses.']
text+=['','## Fresh CPU and matched MPSGraph comparison','','Times are process-median microseconds from five shuffled rounds of fresh processes. GPU uploads include allocation and a CPU copy into a fresh owned shared buffer for both implementations. No reuse of a mutable host pointer, output readback, pipeline preparation or graph compilation is included. CPU has no upload mode. This is one unmasked single-head bias-free pre-RMSNorm transformer block with a 4× ReLU FFN, not end-to-end LLM generation.','','| Tokens × width | Mode | CPU µs | Kalandra Metal µs | MPSGraph µs | Metal / MPS time |','|---|---|---:|---:|---:|---:|']
for m,d in shapes:
    for mode,label in enumerate(('resident','activation','all uploads')):
        cpu=summary['cpu',m,d,0];gpu=summary['shared',m,d,mode];mps=summary['mps-shared',m,d,mode]
        text.append(f'| {m} × {d} | {label} | {cpu/1000:.2f} | {gpu/1000:.2f} | {mps/1000:.2f} | {gpu/mps:.2f}× |')
text+=['','## Qualification and interpretation','','- All Kalandra output cells are bit-identical to the frozen baseline across 13 regular/odd shapes and all residency modes. Both MPS storage modes satisfy the independent propagated numerical bounds; vendor arithmetic is not asserted bit-identical.',
       '- Upload lifecycle, ten-format/rank 0–8 storage, compute and numeric tests pass in debug, optimized, ASan/UBSan and TSan builds. Prepared-graph aliases, cancellation, allocation failure, concurrent invocations and generated-source checks pass in all four modes.',
       '- The APFloat suite passes 1,745 cases / 1,769,284 cell and aggregate checks per mode. The existing compute limits and 4–6 submissions, 12 dispatches remain. All measured submission counts are audited: each nonempty shared upload contributes zero GPU submissions instead of one, so all-upload calls fall from 11–13 to 4–6 submissions.',
       '- Uploads retain their own snapshot and CPU lease, yield after at most 1 MiB of copying, and check cancellation before chunks and during publication. Driver allocation is not preemptible; this is no wall-clock deadline guarantee.',
       '- The acknowledged shared-storage and unbounded direct-copy screens are retained separately. Neither is the production acceptance implementation. They motivate the bounded-copy path but do not replace its measured results.',
       '- Resident-only regressions and mixed-sign blocks are shown rather than hidden in aggregate speedups. Transfer gains do not establish GPU parity for tiny resident work or parity with MPSGraph.',
       '- MPSGraph remains a benchmark-only dependency. Production uses Kalandra’s own numerical kernels. No tests, builds or plotting ran alongside acceptance timing.',
       '', 'The first new upload test incorrectly expected a deadline kind on the child task; the established runtime reports cancellation on the child and the deadline on its group. The assertion was corrected to check both. No deadline behavior changed. An overly broad first CTest selection was stopped after the intended tests; the remaining numeric TSan case and corrected upload suites were completed separately.',
       '', '## Evidence and reproduction','']
text += [f'- [{name}]({name})' for name in names]
text+=['','Use `native/benchmarks/upload_experiment.py` for the isolated screens, `native/benchmarks/upload_cache_control.py` for the rejected cache experiment, and `native/benchmarks/upload_acceptance.py` for `paired` (5 blocks), `storage` (3), `matched` (5) and `repeat` (5). Each raw record preserves commands, calibration, every measured/warmup sample, verification metrics and SHA-256 provenance. The shared acceptance build directory is `benchmarks/.build/gpu-upload-2026-10-29/acceptance`. Render with this script after measurements. See [the lifecycle design](../../docs/design/gpu-owned-upload-snapshots.md).','']
base.with_suffix('.md').write_text('\n'.join(text))
base.with_suffix('.json').write_text(json.dumps(dict(evidence={n:sha(out/n) for n in names},artifact_audit='passed',source_audit='paired acceptance and qualification passed',submissions_per_invocation=submission_counts,renderer_sha256=sha(Path(__file__))),indent=2)+'\n')
print(base.with_suffix('.md'))
