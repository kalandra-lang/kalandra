"""Audit prepared execution evidence and render its runtime comparison."""
import collections,hashlib,json,statistics
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parents[3];out=root/'benchmarks/results'
names=['transformer-prepared-full-block-2026-10-28.json','transformer-prepared-paired-2026-10-28.json','transformer-prepared-profile-2026-10-28.json']
full,paired,profile=[json.loads((out/name).read_text()) for name in names]
assert paired['current_evidence_sha256']==hashlib.sha256((out/names[0]).read_bytes()).hexdigest()
assert profile['acceptance_sha256']==paired['current_evidence_sha256']
for data in (full,paired,profile):
    for key,value in data['artifact_sha256'].items():
        f=Path(key);f=f if f.is_absolute() else root/f
        assert hashlib.sha256(f.read_bytes()).hexdigest()==value,f
for key,value in full['source_sha256'].items():
    f=Path(key);f=f if f.is_absolute() else root/f
    assert hashlib.sha256(f.read_bytes()).hexdigest()==value,f
shapes=[(1,64),(16,64),(64,64),(1,256),(16,256),(64,256),(1,512),(16,512),(64,512)]
groups=collections.defaultdict(list)
for row in paired['pairs']:groups[tuple(row['group'])].append(row)
assert len(groups)==27 and all(len(v)==5 for v in groups.values())
assert all(row['time_reduction']>0 for row in paired['pairs'])
def summary(m,d,mode):
    rows=groups[m,d,mode]
    return [statistics.median(r[k] for r in rows) for k in ('baseline_ns','current_ns','time_reduction')]
def control(m,d,mode):return {r['backend']:r['median_ns'] for r in full['summary'] if (r['m'],r['d'],r['mode'])==(m,d,mode)}
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':11,'text.color':'#eeedf2','axes.labelcolor':'#b8b5c6','xtick.color':'#aaa6b9','ytick.color':'#eeedf2','axes.edgecolor':'#292632','savefig.facecolor':'#0d0c12'})
fig,axes=plt.subplots(1,2,figsize=(13.5,9.6),facecolor='#0d0c12',sharey=True)
for ax,mode,title in zip(axes,(0,2),('All inputs resident','All seven inputs uploaded per call')):
    ax.set_facecolor('#0d0c12')
    for i,(m,d) in enumerate(shapes):
        before,after,reduction=summary(m,d,mode)
        for y,t,color in [(i-.17,before,'#7d8493'),(i+.17,after,'#b398ff')]:
            ax.barh(y,t/1e6,height=.27,color=color,zorder=3)
            ax.text(t/1e6+.06,y,f'{t/1e6:.2f}',va='center',fontsize=10,color='#ddd9e7')
    ax.set_title(title,loc='left',pad=17,fontweight='bold',fontsize=13)
    ax.set_yticks(range(len(shapes)),[f'{m} × {d}' for m,d in shapes]);ax.invert_yaxis()
    ax.set_xlim(0,max(summary(m,d,mode)[0] for m,d in shapes)/1e6*1.2)
    ax.grid(axis='x',color='#282430',zorder=0);ax.set_axisbelow(True)
    ax.set_xlabel('Milliseconds per complete invocation',labelpad=13)
    for edge in ('top','right','left'):ax.spines[edge].set_visible(False)
    ax.tick_params(axis='y',length=0,pad=12)
axes[0].set_ylabel('Rows × model width',labelpad=17)
fig.text(.065,.955,'Prepared GPU execution',fontsize=25,fontweight='bold')
fig.text(.065,.915,'Same transformer block and identical generated shader source · lower is faster',fontsize=12,color='#b8b5c6')
from matplotlib.patches import Patch
fig.legend(handles=[Patch(color='#7d8493',label='Baseline · 27522768'),Patch(color='#b398ff',label='Prepared + bounded batching')],loc='upper left',bbox_to_anchor=(.06,.892),frameon=False,ncol=2)
fig.text(.065,.075,'Five ABBA/BAAB blocks per case · two warmup and three measured batches per process',fontsize=10,color='#b8b5c6')
fig.text(.065,.05,'Full synchronous API, acknowledgement and retirement · no output readback in timing · preparation excluded',fontsize=10,color='#b8b5c6')
fig.text(.065,.025,'Apple M2 Max · macOS 27 · 28 Oct 2026 · all 27 case medians improved; small cases still favor CPU',fontsize=10,color='#b8b5c6')
fig.subplots_adjust(left=.12,right=.98,top=.80,bottom=.15,wspace=.19)
stem=out/'gpu-prepared-execution-2026-10-28'
fig.savefig(stem.with_suffix('.png'),dpi=160);fig.savefig(stem.with_suffix('.svg'));plt.close(fig)
reductions=[summary(*key)[2] for key in groups]
prep=[s['preparation_ns'] for r in full['observations'] if r['backend']=='gpu' for s in r['samples'] if s['round']>=0]
lines=['# Prepared typed GPU execution — 28 October 2026','',
'Full-block API time fell **18–66% across all 27 workload/residency case medians** against frozen `27522768`. Every one of the 135 paired blocks improved. The generated shader source is unchanged: this measures execution/ownership improvements, not a different numerical algorithm.','',
'![Frozen baseline versus prepared execution](gpu-prepared-execution-2026-10-28.png)','',
'## Implementation and semantics','',
'- Validated immutable instructions, kernel owners, scalar schemas, use counts and map argument templates are prepared once. Each invocation owns its values, use counters, raw scalar captures and deferred-retirement list. Generated host frames reuse plans only on the matching registry/device; scalar bindings are refreshed on every call.',
'- Batches contain at most four logical stages and 64 Mi estimated work. A single larger dispatch remains individually acknowledged. Emitted fault predicates prove eligible float32 maps non-faulting; hardware float32 products and native transforms have known arithmetic behavior. Host RMS epsilon must also be positive and finite. Checked/unproved arithmetic, device epsilon, readbacks and branches retain boundaries.',
'- All parameters, scratch, indirect resources and deferred last-use owners remain charged through ACK. A later host preparation/shape fault acknowledges prior encoded work first; an earlier device failure has precedence. Inputs are never donated. Cancellation is cooperative: an already submitted chunk settles on ACK, potentially after four short stages rather than one. Tight workspace budgets keep individual stages.',
'- This is a private native runtime API change. There is no new language syntax, relaxed arithmetic, vendor dependency or ownership requirement. The CPU full-block LLVM is byte-identical to the prior artifact.','',
'## Frozen before/after comparison','',
'Times below are medians of paired block means. Percentage reductions are medians of within-block ratios, so rounding and different median operations can make them differ slightly from the displayed time ratio. All cells use identical iterations within each ABBA/BAAB pair; calibration targets the faster implementation.','',
'| Rows × width | Resident before → after (ms) | Reduction | Activation upload reduction | All uploads reduction |','|---|---:|---:|---:|---:|']
for m,d in shapes:
    a,b,r=summary(m,d,0)
    lines.append(f'| {m} × {d} | {a/1e6:.3f} → {b/1e6:.3f} | {100*r:.1f}% | {100*summary(m,d,1)[2]:.1f}% | {100*summary(m,d,2)[2]:.1f}% |')
lines+=['','Immutable plan preparation alone takes a median **%.2f µs** across fresh processes (range %.2f–%.2f µs), excluding pipeline loading. It is outside warm timing on both paths; per-invocation capture binding, task/group creation, GPU ACK and retirement remain inside. This is not executable launch latency.'%(statistics.median(prep)/1000,min(prep)/1000,max(prep)/1000),'',
'## CPU and optimized-library control','',
'These are a separate counterbalanced CPU/Metal/MPSGraph run; do not splice their times into the paired baseline dataset. MPSGraph is a benchmark-only library control and may use different reduction/fusion strategies within the verified per-cell forward-error envelope.','',
'| Rows × width | CPU (ms) | Metal (ms) | MPSGraph (ms) | CPU / Metal speedup | Metal / MPS time |','|---|---:|---:|---:|---:|---:|']
for m,d in shapes:
    c=control(m,d,0);lines.append(f"| {m} × {d} | {c['cpu']/1e6:.3f} | {c['gpu']/1e6:.3f} | {c['mps']/1e6:.3f} | {c['cpu']/c['gpu']:.2f}× | {c['gpu']/c['mps']:.2f}× |")
lines+=['','Only three of nine resident cases beat CPU. With all seven uploads included, only the two 512-wide batched cases beat CPU. MPSGraph remains faster in every GPU case; the retained “Failed to import MPS module” framework diagnostic did not prevent execution or output verification. No universal C++/Rust or end-to-end LLM claim is made.','',
'## Isolated instrumentation','',
'Profiled wall times are diagnostics and are not the acceptance times above. Recorded encoding counters omit some helper/setup cost; unaccounted wall time is not proven to be solely condition-variable waiting.','',
'| Resident case | Submissions / dispatches | Allocation/admission checkpoints | Wall (ms) | GPU command time (ms) | Recorded encoding (ms) |','|---|---:|---:|---:|---:|---:|']
for m in (1,64):
    rows=[s for r in profile['observations'] if r['m']==m and r['mode']==0 for s in r['samples'] if s['round']>=0]
    med=lambda k:statistics.median(s[k]/3 for s in rows)
    lines.append(f'| {m} × 512 | {med("submissions"):.0f} / {med("dispatches"):.0f} | {med("attempts"):.0f} | {med("ns")/1e6:.3f} | {med("gpu_ns")/1e6:.3f} | {med("encoding_ns")/1e6:.3f} |')
lines+=['','The previous profile used 16 submissions and 235 checkpoints per invocation. Large products exceed the batching work budget, so 64 × 512 still needs eight ACKs. The next targets are projection/pointwise fusion and invocation-local temporary/parameter reuse, while preserving adjacent-pair reductions and checked-fault order. The remaining vendor gap includes both runtime costs and differing library kernel/fusion/math strategies; this profile does not isolate their individual contribution.','',
'## Qualification','',
'- Full model: 91 verification records, 476,455 output-cell checks over 13 shapes and all residency modes; strict nominal and independently propagated float64 error bounds. Frozen baseline/candidate dumps additionally match bit-for-bit for all 67,392 cells across nine non-dyadic comparison shapes.',
'- Prepared lifecycle: optimized, debug, ASan/UBSan and TSan; changing epsilon/scale, eight concurrent calls, immutable original inputs, all 127 hot-path injected allocation checkpoints, 12 held cancellation/deadline/device-failure boundaries, deferred host-fault precedence, core destruction, dropped plan handles, malformed captures, invalid epsilon and 16/32/64 KiB workspace budgets. A checked float→int→float source map remains unbatched and faults before later work.',
'- Independent GPU row math: 48 configurations / 145,332 cell checks and device-derived epsilon/fault checks across four modes.',
'- Typed hardware float32 matrices: 1,724 APFloat cases / 1,598,840 cell/aggregate checks per mode across all four modes.',
'- Native source debug/optimized/sanitized, all-ten-type matrix-program optimized/sanitized, private numerical runtime optimized/sanitized and legacy graph batching gates passed. Relocated distribution status is recorded in the qualification manifest.',
'- Profiling, correctness tests, compilation and rendering were separate from acceptance timing. No competing build/test work was launched during either timing run.','',
'## Evidence and reproduction','']
for name in names:lines.append(f'- [{name}]({name}) — SHA256 `{hashlib.sha256((out/name).read_bytes()).hexdigest()}`')
lines+=['- [Qualification manifest](transformer-prepared-qualification-2026-10-28.json).','',
'Baseline compiler/runtime/driver/header/executable were frozen before edits in `benchmarks/.build/typed-execution-2026-10-28/baseline`; its manifest is embedded in paired evidence. The baseline shader bundle is `benchmarks/.build/transformer-hardware-final/bundle`. Ignored build artifacts are not checked into Git; regenerate the baseline from `27522768` for a new machine. Exact commands, source/artifact hashes, toolchain versions, calibration, every warmup/measured batch and retained framework diagnostics are in the evidence.','',
'```sh','python3 native/benchmarks/transformer_block.py --build-dir benchmarks/.build/typed-execution-2026-10-28/acceptance --output benchmarks/results/transformer-prepared-full-block-2026-10-28.json --blocks 5 --target-ms 30','python3 native/benchmarks/prepared_execution.py --baseline benchmarks/.build/typed-execution-2026-10-28/baseline --baseline-bundle benchmarks/.build/transformer-hardware-final/bundle --current-evidence benchmarks/results/transformer-prepared-full-block-2026-10-28.json --output benchmarks/results/transformer-prepared-paired-2026-10-28.json --blocks 5','python3 native/benchmarks/transformer_profile.py --evidence benchmarks/results/transformer-prepared-full-block-2026-10-28.json --build-dir benchmarks/.build/typed-execution-2026-10-28/profile --output benchmarks/results/transformer-prepared-profile-2026-10-28.json','```','',
'The runners refuse to overwrite evidence; use fresh paths when repeating. Render with `native/benchmarks/fair/prepared_report.py` after the evidence audit.']
stem.with_suffix('.md').write_text('\n'.join(lines)+'\n')
stem.with_suffix('.json').write_text(json.dumps(dict(evidence_sha256={name:hashlib.sha256((out/name).read_bytes()).hexdigest() for name in names},min_reduction=min(reductions),max_reduction=max(reductions),all_135_blocks_improved=True,renderer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()),indent=2)+'\n')
print(stem.with_suffix('.md'))
