"""Audit and render frozen GPU command-progression evidence, including the rejected protocol."""
import collections, hashlib, json, statistics
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parents[3];out=root/'benchmarks/results'
names=[f'transformer-progression-{kind}-2026-10-28.json' for kind in ('full-block','paired','profile','events','qualification','retention')]
full,paired,profile,events,qualification,retention=[json.loads((out/name).read_text()) for name in names]
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
assert paired['current_evidence_sha256']==sha(out/names[0])==profile['acceptance_sha256']
for data in (full,paired,profile,events,retention):
    for name,value in data['artifact_sha256'].items():
        file=Path(name);file=file if file.is_absolute() else root/file
        assert sha(file)==value,file
for name,value in full['source_sha256'].items():
    file=Path(name);file=file if file.is_absolute() else root/file
    assert sha(file)==value,file
assert all(r['relation']=='identical' for r in paired['shader_source_relationship'].values())
shapes=[(m,d) for d in (64,256,512) for m in (1,16,64)]
def grouped(data):
    result=collections.defaultdict(list)
    for row in data['pairs']:result[tuple(row['group'])].append(row)
    assert len(result)==27
    return result
groups=grouped(paired);event_groups=grouped(events);retention_groups=grouped(retention)
assert all(len(rows)==5 for rows in groups.values())
def reduction(key,source=groups):return statistics.median(r['time_reduction'] for r in source[key])*100
def timing(key,field):return statistics.median(r[field] for r in groups[key])/1000
def control(m,d,mode=0):return {r['backend']:r['median_ns'] for r in full['summary'] if (r['m'],r['d'],r['mode'])==(m,d,mode)}
wins=sum(reduction(key)>0 for key in groups);consistent=sum(all(r['time_reduction']>0 for r in rows) for rows in groups.values())
event_losses=sum(reduction(key,event_groups)<0 for key in event_groups)
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':11,'text.color':'#eeedf2','axes.labelcolor':'#bbb7c9','xtick.color':'#aaa6b9','ytick.color':'#eeedf2','axes.edgecolor':'#292632','savefig.facecolor':'#0d0c12'})
fig,ax=plt.subplots(figsize=(12.8,9),facecolor='#0d0c12');ax.set_facecolor('#0d0c12')
values_all=[]
for mode,(color,label) in enumerate(zip(['#b398ff','#8da9df','#e6ac86'],['Resident','Activation upload','All seven uploads'])):
    values=[reduction((m,d,mode)) for m,d in shapes];values_all+=values
    ys=[i+(mode-1)*.23 for i in range(9)]
    ax.barh(ys,values,height=.19,color=color,label=label,zorder=3)
    for y,value in zip(ys,values):ax.text(value+(.13 if value>=0 else -.13),y,f'{value:+.1f}%',ha='left' if value>=0 else 'right',va='center',fontsize=9)
ax.set_yticks(range(9),[f'{m} × {d}' for m,d in shapes]);ax.invert_yaxis()
ax.set_xlim(min(-2,min(values_all)-1.7),max(6,max(values_all)+1.7))
ax.axvline(0,color='#a3a0ae',linewidth=1);ax.grid(axis='x',color='#282430',zorder=0);ax.set_axisbelow(True)
ax.set_xlabel('Paired median API-time reduction (%) · positive is faster',labelpad=14);ax.set_ylabel('Rows × model width',labelpad=15)
for edge in ('top','right','left'):ax.spines[edge].set_visible(False)
ax.tick_params(axis='y',length=0,pad=12);ax.legend(loc='lower right',frameon=False,fontsize=10)
fig.text(.07,.954,'GPU command progression',fontsize=25,fontweight='bold')
fig.text(.07,.912,'Completion-driven bounded chunks vs. frozen 090d4087 · numerical shader source unchanged',fontsize=11,color='#bbb7c9')
fig.text(.07,.065,'Five ABBA/BAAB blocks · full synchronous API/ACK/retirement · no timed output readback · setup excluded',fontsize=10,color='#bbb7c9')
fig.text(.07,.035,'Apple M2 Max · macOS 27 · 28 Oct 2026 · every case and regression retained',fontsize=10,color='#bbb7c9')
fig.subplots_adjust(left=.15,right=.97,top=.86,bottom=.17)
stem=out/'gpu-progression-execution-2026-10-28'
fig.savefig(stem.with_suffix('.png'),dpi=160);fig.savefig(stem.with_suffix('.svg'));plt.close(fig)
large=control(64,512)
lines=['# GPU command progression','',
       f'Against frozen `090d4087`, **{wins}/27 case medians improve**, with {consistent} positive in all five paired blocks. Resident 64 × 512 takes **{reduction((64,512,0)):.2f}% less API time**. In the independent control run it is {large["cpu"]/large["gpu"]:.2f}× faster than the Kalandra CPU block, and takes {large["gpu"]/large["mps"]:.2f}× the MPSGraph time. These are transformer-block results, not whole-language or full-LLM rankings.','',
       'The accepted path pre-encodes eligible float32 chunks and lets completion callbacks check cancellation/faults and commit the next chunk. One external lease replaces repeated task-scheduler resumes. Four-stage/64 Mi-work quanta, immutable inputs, arithmetic order and first-fault behavior remain. Pipeline and backing ownership is retained explicitly through acknowledgement, permitting unretained Metal references in the admitted path.','',
       'Finite command-pool capacity is reserved before encoding. Long graphs, resource contention, tight budgets, unproved/checked math, other numeric formats and control/readback barriers use the existing streaming path. A 513-map / 129-chunk regression proves this is an optimization guard, not a language limit.','',
       '## Frozen paired results','',
       'Five adjacent ABBA/BAAB fresh-process blocks use identical iterations within each pair, calibrated to the faster implementation. Times are medians of paired block means; reductions are medians of within-block ratios. Both include hot metadata validation, capture binding, allocations, task/group setup, all submitted GPU work, acknowledgement and retirement. Neither includes pipeline loading, plan preparation or output readback. Timed checksums validate result extents; actual numerical cells are independently checked outside timing.','',
       '| Rows × width | Residency | Before (µs) | After (µs) | Time reduction | Faster blocks |','|---|---|---:|---:|---:|---:|']
labels=['Resident','Activation upload','All seven uploads']
for m,d in shapes:
    for mode in range(3):
        key=(m,d,mode);positive=sum(r['time_reduction']>0 for r in groups[key])
        lines.append(f'| {m} × {d} | {labels[mode]} | {timing(key,"baseline_ns"):.2f} | {timing(key,"current_ns"):.2f} | {reduction(key):+.2f}% | {positive}/5 |')
lines+=['','Individual negative blocks and outliers remain in the raw records. Mixed signs are not evidence of a reliable per-case win. No gain is attributed solely to one subchange: pre-encoding, callback progression, pool sizing and reference retention are measured together.','',
        '## Rejected event-gated prototype','',
        f'The isolated replay regresses **{event_losses}/27 case medians**. One pre-encoded command with CPU/GPU shared-event checkpoints and zeroable indirect grids has fewer command submissions, but worse elapsed API time. It is not enabled in production. Its numerical outputs match the frozen executable bit for bit, and its independent Metal witness verifies grid suppression with API/shader validation. Full cancellation/device-failure qualification is not claimed for this rejected implementation.','',
        '| Rows × width | Resident change | Activation change | All-upload change |','|---|---:|---:|---:|']
for m,d in shapes:lines.append(f'| {m} × {d} | '+ ' | '.join(f'{reduction((m,d,mode),event_groups):+.2f}%' for mode in range(3))+' |')
lines+=['','Positive remains less API time; negative means regression. Event measurements use their own adjacent frozen baseline controls. The tiny gate witness is a synchronization experiment, not an application benchmark.','',
        '## Retention isolation','',
        'A second five-block paired control changes only retained versus unretained references in the same compiled command-chain runtime and shader bundle. Here positive means retained references are faster. Retention is not a general fix for the all-upload regression: results vary by case. Resident 64 × 512 is slower with retained references in all five blocks; the default remains lease-owned unretained references. The 64 × 256 all-upload regression against the original runtime remains visible rather than being routed around by benchmark case.','',
        '| Rows × width | Resident retained vs unretained | Activation | All upload |','|---|---:|---:|---:|']
for m,d in shapes:lines.append(f'| {m} × {d} | '+ ' | '.join(f'{reduction((m,d,mode),retention_groups):+.2f}%' for mode in range(3))+' |')
lines+=['',
        '## CPU and vendor controls','',
        'A separate five-block counterbalanced CPU/Metal/MPSGraph study measures the same bias-free single-head transformer block: pre-RMSNorm, separate Q/K/V projections, unmasked attention, output projection, a four-times-width ReLU FFN and two residuals. This is not token generation or a full model. Production code uses its own kernels; MPSGraph is benchmark-only. Vendor reduction/FMA choices need not be bit-identical to Kalandra’s separately rounded products and adjacent-pair tree; all outputs meet the independent per-cell oracle/bound.','',
        '| Rows × width | CPU (µs) | Metal resident (µs) | MPSGraph resident (µs) | CPU / Metal | Metal / MPS |','|---|---:|---:|---:|---:|---:|']
for m,d in shapes:
    c=control(m,d);lines.append(f'| {m} × {d} | {c["cpu"]/1000:.2f} | {c["gpu"]/1000:.2f} | {c["mps"]/1000:.2f} | {c["cpu"]/c["gpu"]:.2f}× | {c["gpu"]/c["mps"]:.2f}× |')
lines+=['','Small workloads still favor CPU. MPSGraph remains ahead. Submission progression alone does not close that gap. Input transfers still use individual staged blits; a unified-memory copy policy is a separate experiment requiring matched vendor controls and qualification. Further compiler/kernel fusion and call-site cancellability analysis are architectural targets; simply batching unbounded work or reading an unsynchronized CPU/GPU cancellation word is not the accepted solution.','',
        '## Qualification and profiling','',
        f'- Independent control validation: {len(full["verification"])} records / {sum(r["cells"] for r in full["verification"]):,} cells; odd shapes and all residency modes included.',
        '- Frozen GPU output dumps match bit for bit; all numerical shader source hashes are identical. The compiler binary and CPU full-block LLVM are unchanged.',
        '- Optimized, debug, ASan/UBSan and TSan prepared-graph tests: 116 small-chain and 119 large-chain injected allocation checkpoints per mode, plus 112 streaming control checkpoints. Held cancellation, recorded deadline and device failure cover all four and seven chunk boundaries. Higher ranks, 513-map fallback, private reuse/aliases, dropped executable handles, concurrent invocations, core teardown and 16/32/64 KiB fallback pass. Compiler-generated source entry points run in all four modes.',
        '- Typed hardware matrices: 1,724 APFloat cases / 1,598,840 cell/aggregate checks per mode. All-ten-type matrix CLI tests, legacy graph batching, tensor transforms, external leases, CPU source regression and relocated installation passed. A matrix CLI harness was updated to accept the shader-validation informational notice; numeric assertions remain strict.',
        '- Profiling is a separate explicitly instrumented run. GPU and recorded encoding intervals are not an exhaustive exclusive-time breakdown; unclassified wall time must not be called entirely scheduler or driver overhead. Acceptance runs overlap no builds, tests, plots or profiling.','']
for m,d in [(1,512),(64,512)]:
    samples=[(r,s) for r in profile['observations'] if (r['m'],r['d'],r['mode'])==(m,d,0) for s in r['samples'] if s['round']>=0]
    med=lambda field:statistics.median(s[field]/r['iterations'] for r,s in samples)
    lines.append(f'- Profile {m} × {d} resident: wall {med("ns")/1e6:.3f} ms, GPU command intervals {med("gpu_ns")/1e6:.3f} ms, recorded encoding {med("encoding_ns")/1e6:.3f} ms, {med("attempts"):.0f} admission attempts, {med("submissions"):.0f} submissions / {med("dispatches"):.0f} dispatches.')
lines+=['','## Evidence and reproduction','',
        'Baseline artifacts were frozen before edits under `benchmarks/.build/gpu-progression-2026-10-28/baseline`. Its manifest is embedded in paired evidence. The baseline shader bundle is `benchmarks/.build/gpu-fusion-2026-10-28/acceptance/bundle`. Ignored binaries are not committed; rebuild `090d4087` in a separate directory to regenerate them. All samples, calibration, exact commands and artifact/source/toolchain hashes are retained. The event runner applies its archived patch to that exact commit in an isolated source tree.','']
for name in names:lines.append(f'- [{name}]({name}) — SHA256 `{sha(out/name)}`')
lines+=['','```sh',
'python3 native/benchmarks/transformer_block.py --build-dir benchmarks/.build/gpu-progression-2026-10-28/acceptance --output benchmarks/results/transformer-progression-full-block-2026-10-28.json --blocks 5 --target-ms 30',
'python3 native/benchmarks/prepared_execution.py --baseline benchmarks/.build/gpu-progression-2026-10-28/baseline --baseline-bundle benchmarks/.build/gpu-fusion-2026-10-28/acceptance/bundle --current-evidence benchmarks/results/transformer-progression-full-block-2026-10-28.json --output benchmarks/results/transformer-progression-paired-2026-10-28.json --blocks 5',
'python3 native/benchmarks/event_progression.py --baseline benchmarks/.build/gpu-progression-2026-10-28/baseline --bundle benchmarks/.build/gpu-fusion-2026-10-28/acceptance/bundle --build-dir benchmarks/.build/gpu-progression-2026-10-28/event-replay --output benchmarks/results/transformer-progression-events-2026-10-28.json --blocks 3',
'python3 native/benchmarks/transformer_profile.py --evidence benchmarks/results/transformer-progression-full-block-2026-10-28.json --build-dir benchmarks/.build/gpu-progression-2026-10-28/profile --output benchmarks/results/transformer-progression-profile-2026-10-28.json --dispatches 14 --large-submissions 7','```']
stem.with_suffix('.md').write_text('\n'.join(lines)+'\n')
stem.with_suffix('.json').write_text(json.dumps(dict(evidence_sha256={name:sha(out/name) for name in names},positive_medians=wins,negative_medians=27-wins,all_positive_blocks=consistent,event_regressions=event_losses,renderer_sha256=sha(Path(__file__))),indent=2)+'\n')
print(stem.with_suffix('.md'))
