"""Audit and render the complete matrix-epilogue/private-reuse comparison."""
import collections,hashlib,json,statistics
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parents[3];out=root/'benchmarks/results'
names=['transformer-fusion-full-block-2026-10-28.json','transformer-fusion-paired-2026-10-28.json','transformer-fusion-profile-2026-10-28.json','transformer-fusion-setup-2026-10-28.json']
full,paired,profile,setup=[json.loads((out/name).read_text()) for name in names]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert paired['current_evidence_sha256']==sha(out/names[0])==profile['acceptance_sha256']
for data in (full,paired,profile,setup):
    for name,value in data['artifact_sha256'].items():
        f=Path(name);f=f if f.is_absolute() else root/f;assert sha(f)==value,f
for name,value in full['source_sha256'].items():
    f=Path(name);f=f if f.is_absolute() else root/f;assert sha(f)==value,f
shapes=[(1,64),(16,64),(64,64),(1,256),(16,256),(64,256),(1,512),(16,512),(64,512)]
groups=collections.defaultdict(list)
for r in paired['pairs']:groups[tuple(r['group'])].append(r)
assert len(groups)==27 and all(len(rows)==5 for rows in groups.values())
def reduction(m,d,mode):return statistics.median(r['time_reduction'] for r in groups[m,d,mode])*100
def control(m,d,mode=0):return {r['backend']:r['median_ns'] for r in full['summary'] if (r['m'],r['d'],r['mode'])==(m,d,mode)}
wins=sum(reduction(*k)>0 for k in groups);losses=27-wins
consistent=sum(all(r['time_reduction']>0 for r in rows) for rows in groups.values())
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':11,'text.color':'#eeedf2','axes.labelcolor':'#b8b5c6','xtick.color':'#aaa6b9','ytick.color':'#eeedf2','axes.edgecolor':'#292632','savefig.facecolor':'#0d0c12'})
fig,ax=plt.subplots(figsize=(12.4,9),facecolor='#0d0c12');ax.set_facecolor('#0d0c12')
colors=['#b398ff','#8da9df','#e6ac86']
labels=['Resident','Activation upload','All seven uploads']
for mode in range(3):
    values=[reduction(m,d,mode) for m,d in shapes];y=[i+(mode-1)*.23 for i in range(9)]
    ax.barh(y,values,height=.19,color=colors[mode],label=labels[mode],zorder=3)
    for yy,v in zip(y,values):ax.text(v+(.13 if v>=0 else -.13),yy,f'{v:+.1f}%',ha='left' if v>=0 else 'right',va='center',fontsize=9)
ax.set_yticks(range(9),[f'{m} × {d}' for m,d in shapes]);ax.invert_yaxis();ax.set_xlim(-4,12)
ax.axvline(0,color='#a3a0ae',linewidth=1);ax.grid(axis='x',color='#282430',zorder=0);ax.set_axisbelow(True)
ax.set_xlabel('Paired median API-time reduction (%) · positive is faster',labelpad=14);ax.set_ylabel('Rows × model width',labelpad=15)
for edge in ('top','right','left'):ax.spines[edge].set_visible(False)
ax.tick_params(axis='y',length=0,pad=12)
fig.text(.07,.954,'Matrix epilogues + private reuse',fontsize=24,fontweight='bold')
fig.text(.07,.911,'Against frozen prepared runtime ca7b5705 · complete transformer block · all cases retained',fontsize=11,color='#b8b5c6')
ax.legend(loc='lower left',bbox_to_anchor=(-.01,1.015),ncol=3,frameon=False,fontsize=10)
fig.text(.07,.075,f'{wins}/27 positive case medians · {losses} small negative medians shown · {consistent} cases improved in all five blocks',fontsize=10,color='#b8b5c6')
fig.text(.07,.05,'Five ABBA/BAAB blocks · full synchronous API/ACK/retirement · no output readback · setup excluded',fontsize=10,color='#b8b5c6')
fig.text(.07,.025,'Apple M2 Max · macOS 27 · 28 Oct 2026 · small workloads still favor CPU; MPSGraph remains faster',fontsize=10,color='#b8b5c6')
fig.subplots_adjust(left=.15,right=.97,top=.80,bottom=.17)
stem=out/'gpu-fusion-execution-2026-10-28';fig.savefig(stem.with_suffix('.png'),dpi=160);fig.savefig(stem.with_suffix('.svg'));plt.close(fig)
lines=['# Typed matrix epilogues and private intermediate reuse','',
'Against frozen `ca7b5705`, the largest resident block takes **9.9% less API time**, and its all-inputs-uploaded case takes **5.3% less**. In a separate CPU/Metal/MPSGraph run, resident 64 × 512 is **4.90× faster on Metal than CPU**.','',
f'This is not a universal win: **{wins}/27** case medians improve and **{losses}/27** are 0.2–1.2% slower. Only {consistent} cases improve in every paired block. The negative medians have mixed block signs; their evidence is retained and no broad small-workload speedup is claimed.','',
'![All paired case medians, including losses](gpu-fusion-execution-2026-10-28.png)','',
'## Implementation','',
'- Compiler-proven, non-faulting single-input float32 maps can become matrix epilogues when immediately adjacent to a single-consumer product. The pinned converter’s canonical straight-line form is required; indexing, control flow, stores, types or layouts outside that form keep separate stages. Profiles are emitted only for actual adjacent matrix producers, so ordinary maps avoid unused pipeline creation.',
'- The original matrix shader and reduction tree remain intact. The epilogue operates on the completed float32 value through the same uint bit carriers as the staged map. No FMA contraction or reassociation is enabled. The transformer fuses attention-score scaling and the feed-forward ReLU; residual-add fusion across shape guards is not implemented.',
'- Each invocation tracks private allocation origins. A pointwise map may overwrite a private last-use value only when no other live value aliases its backing. Input aliases remain original inputs; result observation counts as a live use. Checked maps, transforms, original inputs and live/escaping aliases are excluded. Inputs do not gain user-visible mutation or ownership requirements.',
'- Both fused logical stages count toward the existing four-stage quantum. Both parameter buffers stay charged until ACK. A fused oversized product is one isolated dispatch; other work is not appended to it. Cancellation remains cooperative and task settlement waits for the in-flight command. Fault precedence and cleanup are preserved.',
'- The stale LSP `tra` prefix expectation was corrected in `a97e91b3`: both `transfer` and `transpose` are valid. Debug/sanitized protocol tests passed locally; Linux and Windows hosted qualification passed on that correction.','',
'## Frozen paired comparison','',
'Five ABBA/BAAB blocks per shape/residency case use adjacent fresh processes and identical iterations within each pair. Calibration targets the faster implementation. Pipeline loading and immutable plan preparation are excluded; hot capture binding, task/group setup, GPU ACK and retirement are included. Output readback is excluded on both GPU paths.','',
'Times are medians of paired block means; reductions are medians of within-block ratios. These distinct median operations mean a displayed time ratio can differ slightly from the percentage. “Blocks +” counts faster candidate blocks out of five.','',
'| Rows × width | Resident before → after (ms) | Resident reduction | Blocks + | Activation reduction | All-upload reduction |','|---|---:|---:|---:|---:|---:|']
for m,d in shapes:
    rows=groups[m,d,0];a=statistics.median(r['baseline_ns'] for r in rows);b=statistics.median(r['current_ns'] for r in rows)
    lines.append(f'| {m} × {d} | {a/1e6:.3f} → {b/1e6:.3f} | {reduction(m,d,0):+.2f}% | {sum(r["time_reduction"]>0 for r in rows)}/5 | {reduction(m,d,1):+.2f}% | {reduction(m,d,2):+.2f}% |')
lines+=['','Negative medians:','']
for key,rows in sorted(groups.items()):
    value=reduction(*key)
    if value<0:lines.append(f'- {key[0]} × {key[1]}, {labels[key[2]].lower()}: {value:+.2f}%; block range {100*min(r["time_reduction"] for r in rows):+.1f}% to {100*max(r["time_reduction"] for r in rows):+.1f}%.')
lines+=['','Large transients remain visible in raw data, including a −29% block in the 64 × 512 activation-upload case. No outlier was removed and no follow-up run was substituted for a losing case. Five blocks provide limited precision for effects near 1%.','',
'## CPU and optimized-library control','',
'These times come from a separate counterbalanced CPU/Metal/MPSGraph run; they are not spliced into the frozen paired comparison. MPSGraph remains a benchmark-only control with its own fusion/reduction strategies, verified against independent per-cell forward-error bounds.','',
'| Rows × width | CPU (ms) | Metal (ms) | MPSGraph (ms) | CPU / Metal speedup | Metal / MPS time |','|---|---:|---:|---:|---:|---:|']
for m,d in shapes:
    c=control(m,d);lines.append(f'| {m} × {d} | {c["cpu"]/1e6:.3f} | {c["gpu"]/1e6:.3f} | {c["mps"]/1e6:.3f} | {c["cpu"]/c["gpu"]:.2f}× | {c["gpu"]/c["mps"]:.2f}× |')
lines+=['','Small workloads still favor CPU; Metal beats CPU in three resident cases. The largest resident Metal case remains about 4.2× the MPSGraph time. The retained “Failed to import MPS module” framework diagnostic did not prevent execution or verification. This is a bias-free, single-head, unmasked pre-RMSNorm block with unit RMS gain, separate Q/K/V and output projections, attention softmax, two residuals and a 4× ReLU feed-forward layer; it is not token generation, KV caching or an end-to-end LLM.','',
'## Setup and isolated profiling','',
'An independent alternating-order fresh-process setup diagnostic excludes Runtime creation, uploads and process launch. Program/pipeline setup rises from **%.3f ms to %.3f ms**; the extra fused pipeline states cost about **%.3f ms**. Median plan preparation is %.2f µs before and %.2f µs after. This is a warm-throughput optimization with an explicit cold-setup cost.'%tuple([statistics.median(r['pipeline_ns'] for r in setup['observations'] if r['version']==v)/1e6 for v in ['baseline','current']]+[(statistics.median(r['pipeline_ns'] for r in setup['observations'] if r['version']=='current')-statistics.median(r['pipeline_ns'] for r in setup['observations'] if r['version']=='baseline'))/1e6]+[statistics.median(r['plan_ns'] for r in setup['observations'] if r['version']==v)/1000 for v in ['baseline','current']]),'',
'Instrumented wall time below is diagnostic, not acceptance timing. Recorded encoding counters omit some setup and do not form an exhaustive exclusive-time breakdown with GPU time.','',
'| Resident case | Submissions / dispatches | Allocation/admission checkpoints | Wall (ms) | GPU command time (ms) | Recorded encoding (ms) |','|---|---:|---:|---:|---:|---:|']
for m in [1,64]:
    rs=[s for row in profile['observations'] if row['m']==m and row['mode']==0 for s in row['samples'] if s['round']>=0]
    med=lambda k:statistics.median(r[k]/3 for r in rs)
    lines.append(f'| {m} × 512 | {med("submissions"):.0f} / {med("dispatches"):.0f} | {med("attempts"):.0f} | {med("ns")/1e6:.3f} | {med("gpu_ns")/1e6:.3f} | {med("encoding_ns")/1e6:.3f} |')
lines+=['','The prior prepared profile had 16 dispatches, 4/8 submissions and 127/131 checkpoints. The candidate has 14 dispatches, 4/7 submissions and 112/115 checkpoints. Remaining acknowledgement/driver and allocation work is substantial; the profile does not prove all unaccounted wall time is scheduler waiting. The next architectural experiment is GPU-side graph progression with device-visible cancellation/fault gates, retaining first-fault ordering and ACK-scoped retirement. Residual/projection fusion across validated shape guards is another candidate. Neither is claimed implemented here.','',
'## Qualification and evidence','',
'- Full block: 91 verification records / 476,455 output-cell checks across 13 shapes and all residency modes. The frozen baseline/candidate dumps match bit-for-bit across all 67,392 cells in nine non-dyadic comparison shapes.',
'- Four runtime modes (optimized, debug, ASan/UBSan, TSan): 60 fused IEEE/odd-reduction fixtures per mode match staged output bits, with one dispatch per pair. Provenance graphs verify private last-use reuse, live aliases, original-input aliases and escaping results; held backing counts and original input bits are checked.',
'- Four-mode lifecycle: 112 injected hot-path allocation checkpoints, changing epsilon/scale, eight concurrent invocations, held cancellation/deadline/device failure at all four small-block boundaries, deferred host-fault precedence, dropped plan handles, teardown and 16/32/64 KiB budgets.',
'- Independent row math: 48 configurations / 145,332 checks; typed hardware matrices: 1,724 APFloat cases / 1,598,840 cell/aggregate checks per mode across all four modes. Typed all-ten-type matrix programs, legacy graph batching and canonical-epilogue rejection tests passed.',
'- CPU full-block LLVM is byte-identical to the frozen baseline. Original unfused shader source is identical; only the two eligible map libraries append fused entry points. The unchanged primitive source and changed fused-kernel hashes are recorded explicitly.',
'- Relocated distribution qualification is recorded in the qualification manifest. Tests, profiling, setup diagnostics, compilation and plotting were separate from both acceptance timing runs.','']
for name in names:lines.append(f'- [{name}]({name}) — SHA256 `{sha(out/name)}`')
lines+=['- [Qualification manifest](transformer-fusion-qualification-2026-10-28.json).','',
'## Reproduction','',
'Baseline artifacts were frozen before edits in `benchmarks/.build/gpu-fusion-2026-10-28/baseline`; its manifest is embedded in paired evidence. The baseline shader bundle is `benchmarks/.build/typed-execution-2026-10-28/acceptance/bundle`. Ignored build artifacts are not committed: regenerate the runtime from `ca7b5705` on another machine. Exact commands, all samples/calibration, source/artifact hashes and toolchain versions are retained in evidence. Setup-harness source snapshots are in its JSON. Use fresh paths because timing runners refuse overwrites.','',
'```sh','python3 native/benchmarks/transformer_block.py --build-dir benchmarks/.build/gpu-fusion-2026-10-28/acceptance --output benchmarks/results/transformer-fusion-full-block-2026-10-28.json --blocks 5 --target-ms 30','python3 native/benchmarks/prepared_execution.py --baseline benchmarks/.build/gpu-fusion-2026-10-28/baseline --baseline-bundle benchmarks/.build/typed-execution-2026-10-28/acceptance/bundle --current-evidence benchmarks/results/transformer-fusion-full-block-2026-10-28.json --output benchmarks/results/transformer-fusion-paired-2026-10-28.json --blocks 5 --allow-epilogue-extension','python3 native/benchmarks/transformer_profile.py --evidence benchmarks/results/transformer-fusion-full-block-2026-10-28.json --build-dir benchmarks/.build/gpu-fusion-2026-10-28/profile --output benchmarks/results/transformer-fusion-profile-2026-10-28.json --dispatches 14 --large-submissions 7','```']
stem.with_suffix('.md').write_text('\n'.join(lines)+'\n')
stem.with_suffix('.json').write_text(json.dumps(dict(evidence_sha256={n:sha(out/n) for n in names},positive_medians=wins,negative_medians=losses,all_positive_blocks=consistent,renderer_sha256=sha(Path(__file__))),indent=2)+'\n')
print(stem.with_suffix('.md'))
