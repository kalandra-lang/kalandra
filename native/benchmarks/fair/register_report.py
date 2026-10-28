"""Audit and render the strict-arithmetic GPU register-selection study."""
import collections
import hashlib
import json
import statistics
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parents[3]
out = root / 'benchmarks/results'
sha = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
names = [f'transformer-register-{kind}-2026-10-28.json' for kind in
         ('full-block', 'paired', 'profile', 'qualification', 'long-screen', 'wide-screen')]
names += ['gpu-register-rectangles-2026-10-28.json',
          'transformer-dispatch-profile-2026-10-28.json',
          'transformer-dispatch-register-profile-2026-10-28.json']
full, paired, profile, qualification, long, wide, rectangles, before, after = [
    json.loads((out / name).read_text()) for name in names]
assert paired['current_evidence_sha256'] == sha(out / names[0]) == profile['acceptance_sha256']
for data in (full, paired, profile, long, wide, rectangles, before, after):
    for name, value in data['artifact_sha256'].items():
        file = Path(name)
        assert sha(file if file.is_absolute() else root / file) == value, file
for name, value in full['source_sha256'].items():
    assert sha(root / name) == value, name
assert all(r['relation'] == 'identical' for r in paired['shader_source_relationship'].values())
base = root / 'benchmarks/.build/gpu-dispatch-2026-10-28/baseline'
accept = root / 'benchmarks/.build/gpu-dispatch-2026-10-28/acceptance'
assert sha(base / 'compiler') == sha(root / 'build/native/native/kalandra')
cpu_baseline = root / 'benchmarks/.build/gpu-progression-2026-10-28/acceptance/cpu.ll'
cpu_evidence = out / 'transformer-progression-full-block-2026-10-28.json'
assert sha(cpu_baseline) == json.loads(cpu_evidence.read_text())['artifact_sha256'][str(cpu_baseline.relative_to(root))]
assert sha(cpu_baseline) == sha(accept / 'cpu.ll')

def grouped(data):
    groups = collections.defaultdict(list)
    for row in data['pairs']:
        groups[tuple(row['group'])].append(row)
    return groups

groups = grouped(paired)
assert len(groups) == 27 and all(len(rows) == 5 for rows in groups.values())
reduce = lambda rows: statistics.median(r['time_reduction'] for r in rows) * 100
wins = sum(reduce(rows) > 0 for rows in groups.values())
consistent = sum(all(r['time_reduction'] > 0 for r in rows) for rows in groups.values())
shapes = [(m, d) for d in (64, 256, 512) for m in (1, 16, 64)]
changed = {(64,256),(16,512),(64,512)}
affected = [rows for (m,d,mode),rows in groups.items() if (m,d) in changed]
assert len(affected)==9 and all(reduce(rows)>0 for rows in affected)
for data,variant in ((before,2),(after,3)):
    for row in data['observations']:
        if (row['m'],row['d']) in changed:
            for trace in row['traces']:
                assert len(trace['stages'])==14
                assert next(t for t in trace['stages'] if t['pc']==32)['variant']==variant

plt.rcParams.update({'font.family':'DejaVu Sans', 'font.size':11, 'text.color':'#eeedf2',
    'axes.labelcolor':'#bbb7c9', 'xtick.color':'#aaa6b9', 'ytick.color':'#eeedf2',
    'axes.edgecolor':'#292632', 'savefig.facecolor':'#0d0c12'})
fig, ax = plt.subplots(figsize=(12.8, 8.8), facecolor='#0d0c12')
ax.set_facecolor('#0d0c12')
for mode, color, label in [(0,'#ae97ff','Resident inputs'),(1,'#7f9cbf','Activation upload'),(2,'#e6aa82','All inputs uploaded')]:
    values = [reduce(groups[m,d,mode]) for m,d in shapes]
    ys = [i+(mode-1)*.23 for i in range(9)]
    ax.barh(ys, values, height=.20, color=color, label=label)
    for y, value in zip(ys, values):
        ax.text(value+(.18 if value>=0 else -.18), y, f'{value:+.1f}%',
                va='center', ha='left' if value>=0 else 'right', fontsize=9)
ax.set_yticks(range(9), [f'{m} × {d}'+(' *' if (m,d) in changed else '') for m,d in shapes]);ax.invert_yaxis()
ax.axvline(0, color='#797382', lw=.8);ax.grid(axis='x', color='#292632');ax.set_axisbelow(True)
ax.set_xlim(min(-4,min(reduce(r) for r in groups.values())-3), max(12,max(reduce(r) for r in groups.values())+4))
ax.set_xlabel('Reduction in complete execution time (%) · positive is faster', labelpad=14)
for s in ('top','right','left'):ax.spines[s].set_visible(False)
fig.text(.08,.94,'Kalandra GPU · better matrix kernel selection',fontsize=23,weight='bold')
fig.text(.08,.895,'Full transformer block against frozen f28a71f0 · same shaders and arithmetic',fontsize=12,color='#bcb6ca')
ax.legend(loc='upper left',bbox_to_anchor=(0,1.06),ncol=3,frameon=False,fontsize=10)
fig.text(.08,.065,'Five counterbalanced ABBA/BAAB blocks per case · process-median ratios · all 27 results shown',fontsize=10,color='#bcb6ca')
fig.text(.08,.012,'* Different kernel selected; unmarked shapes are unchanged-selection noise controls.',fontsize=9,color='#bcb6ca')
fig.text(.08,.037,'Apple M2 Max · macOS 27 · 28 Oct 2026 · cancellation and ownership checks retained',fontsize=10,color='#bcb6ca')
fig.subplots_adjust(left=.16,right=.94,top=.84,bottom=.16)
stem = out/'gpu-register-selection-2026-10-28'
for ext in ('png','svg'):fig.savefig(stem.with_suffix('.'+ext),dpi=160)

def controls(m,d):
    return {r['backend']:r['median_ns'] for r in full['summary'] if (r['m'],r['d'],r['mode'])==(m,d,0)}
lines = ['# GPU register selection and the MPSGraph gap','',
    f'Against frozen `f28a71f0`, all nine affected complete-block paired medians improve; six improve in every block. Including unchanged-selection controls, {wins}/27 medians are positive. This change broadens selection of an existing strict float32 matrix kernel. It changes no numerical shader, CPU code, cancellation quantum or ownership path.','',
    '## Why MPSGraph is faster','',
    'Apple combines a graph compiler with tuned matrix/reduction kernels and describes stitching adjacent operations into those kernels. Kalandra currently has narrower fusion. See [Apple’s MPSGraph presentation](https://developer.apple.com/videos/play/wwdc2020/10677/?time=1049). This explains the available mechanisms, not an exact attribution of this benchmark’s gap.','',
    'Resident-input measurements exclude transfer cost as an explanation for the resident gap. Kalandra still makes 14 compute dispatches across four or seven explicit submissions, with bounded cancellation checkpoints. Our MPS harness makes one graph encode/commit API call; this does **not** establish its internal command-buffer count. The SDK says graph encoding may call `commitAndContinue`. Do not compare an MPS wrapper count with Kalandra’s actual submissions or interpret the MPS harness’s last command-buffer interval as total graph device time.','',
    'Kalandra keeps separately rounded products and an adjacent-pair reduction tree. Vendor outputs meet the independent numerical bound but need not be bit-identical. The cost of this arithmetic difference has not been isolated. Neither precision relaxation nor removal of cancellation is part of this change.','',
    '## Accepted selection rule','',
    'Register16 now admits at least 8,192 output cells instead of 65,536. Existing guards remain: both output dimensions at least 16, inner dimension 512–4096 and divisible by 64. The explicit scalar reduction roots and existing tile-tail checks are unchanged. The policy applies to ordinary matrix operations as well as prepared graphs, using dimensions rather than benchmark names. Other GPUs have not been physically qualified for this tuning.','',
    'Two isolated three-block screens are retained: admitting only long-inner rectangles, and admitting all qualified rectangles. The broader rule was selected before five-block acceptance. General matrix testing covers skinny/tall rectangles, boundary shapes, partial tiles and non-power-of-two counts of 64-leaf blocks.','',
    '## Complete-block paired acceptance','',
    'Positive percentages mean less time. Counts in parentheses are positive blocks out of five; small changes on unaffected shapes are noise controls, not demonstrated gains. All negative medians remain in the table.','',
    '| Rows × width | Resident | Activation upload | All uploads |','|---|---:|---:|---:|']
for m,d in shapes:
    cells=[]
    for mode in range(3):
        rows=groups[m,d,mode];cells.append(f'{reduce(rows):+.2f}% ({sum(r["time_reduction"]>0 for r in rows)}/5)')
    lines.append(f'| {m} × {d} | '+' | '.join(cells)+' |')
lines += ['','## General matrix acceptance','',
          'Exact dyadic integer contractions verify every output cell in both versions outside timing. Measurements include normal task creation, completion, metadata observation and retirement, with resident inputs. Positive percentages mean less time.','',
          '| M × K × N | Reduction | Positive blocks |','|---|---:|---:|']
for (m,k,n),rows in sorted(grouped(rectangles).items()):
    lines.append(f'| {m} × {k} × {n} | {reduce(rows):+.2f}% | {sum(r["time_reduction"]>0 for r in rows)}/5 |')
lines += ['','## Fresh CPU and MPSGraph controls','',
    'Warm synchronous tensor-result API including ACK/retirement; preparation and output readback excluded for both GPUs. This is a single bias-free unmasked transformer block with 4× ReLU FFN, not model inference or token generation. Five counterbalanced control blocks are separate from the paired baseline study.','',
    '| Rows × width | CPU µs | Kalandra Metal µs | MPSGraph µs | CPU / Metal | Metal / MPS |','|---|---:|---:|---:|---:|---:|']
for m,d in shapes:
    c=controls(m,d);lines.append(f'| {m} × {d} | {c["cpu"]/1000:.2f} | {c["gpu"]/1000:.2f} | {c["mps"]/1000:.2f} | {c["cpu"]/c["gpu"]:.2f}× | {c["gpu"]/c["mps"]:.2f}× |')
lines += ['','## Qualification and diagnostics','',
    f'- All four modes pass {qualification["typed_cases_per_mode"]:,} typed float32 APFloat cases / {qualification["typed_checks_per_mode"]:,} cell/aggregate checks per mode, including partial tiles, IEEE edges, zero reductions and 24/33-block contractions.',
    '- Prepared graphs pass in optimized, debug, ASan/UBSan and TSan modes, including expanded fused IEEE fixtures, held cancellation/deadline/device failures, allocation failures, aliases, concurrent calls, higher ranks and long streaming fallback. No runtime lifecycle code changed.',
    f'- Fresh CPU/Metal/MPS controls verify {len(full["verification"])} records / {sum(r["cells"] for r in full["verification"]):,} cells. Frozen GPU dumps are bit-identical. Compiler binary and full-block CPU LLVM are unchanged; generated numerical shader sources are identical.',
    '- Encoder counters are from isolated instrumented runtime copies. Sampling and clock correlation substantially perturb execution; sampled intervals are diagnostic and cannot be added to or subtracted from uninstrumented wall time. Two clock pairs and raw samples are retained; CPU timestamp units are checked empirically against Mach time rather than assumed. The counter runs verify that projections and the FFN down-projection select Register16 after the change.','']
for m,d in [(1,512),(64,512)]:
    samples=[(r,s) for r in profile['observations'] if (r['m'],r['d'],r['mode'])==(m,d,0) for s in r['samples'] if s['round']>=0]
    med=lambda field:statistics.median(s[field]/r['iterations'] for r,s in samples)
    lines.append(f'- Separate runtime profile {m} × {d}: wall {med("ns")/1e6:.3f} ms; GPU command intervals {med("gpu_ns")/1e6:.3f} ms; recorded encoding {med("encoding_ns")/1e6:.3f} ms; {med("submissions"):.0f} submissions / {med("dispatches"):.0f} dispatches. These intervals do not form an exhaustive exclusive-time breakdown.')
lines += ['','## Remaining work','',
    'MPSGraph remains ahead, and small blocks still favor CPU. The next experiments are contraction-parallel attention-score kernels for small output matrices, additional graph fusion within the existing arithmetic and cancellation contract, and a separately controlled unified-memory transfer policy. Wider asynchronous graph scheduling needs compiler/effect proofs; the accepted change does not remove cancellation boundaries.','',
    '## Evidence and reproduction','',
    'Baseline compiler, driver, archives and generated header were frozen under `benchmarks/.build/gpu-dispatch-2026-10-28/baseline` before edits; CPU LLVM is checked against the prior progression acceptance artifact and its published hash. Rebuild `f28a71f0` to regenerate ignored baseline artifacts; use its `TransformerBlock.k` bundle and typed primitive shader. Every JSON retains commands, raw samples, source/artifact hashes and hardware/toolchain metadata. Diagnostic scripts archive that exact commit and apply instrumentation in an isolated tree. Acceptance overlaps no compilation, tests, profiling or plotting.','']
for name in names:lines.append(f'- [{name}]({name}) — SHA256 `{sha(out/name)}`')
lines += ['','```sh',
    'python3 native/benchmarks/transformer_block.py --build-dir benchmarks/.build/gpu-dispatch-2026-10-28/acceptance --output benchmarks/results/transformer-register-full-block-2026-10-28.json --blocks 5 --target-ms 30',
    'python3 native/benchmarks/prepared_execution.py --baseline benchmarks/.build/gpu-dispatch-2026-10-28/baseline --baseline-bundle benchmarks/.build/gpu-progression-2026-10-28/acceptance/bundle --current-evidence benchmarks/results/transformer-register-full-block-2026-10-28.json --output benchmarks/results/transformer-register-paired-2026-10-28.json --blocks 5',
    'python3 native/benchmarks/register_rectangles.py --baseline benchmarks/.build/gpu-dispatch-2026-10-28/baseline --shader benchmarks/.build/gpu-progression-2026-10-28/typed-qualification/typed/kernel0.metallib --build-dir benchmarks/.build/gpu-dispatch-2026-10-28/rectangles --output benchmarks/results/gpu-register-rectangles-2026-10-28.json --blocks 5',
    'python3 native/benchmarks/transformer_profile.py --evidence benchmarks/results/transformer-register-full-block-2026-10-28.json --build-dir benchmarks/.build/gpu-dispatch-2026-10-28/profile --output benchmarks/results/transformer-register-profile-2026-10-28.json --dispatches 14 --large-submissions 7',
    'python3 native/benchmarks/dispatch_profile.py --baseline benchmarks/.build/gpu-dispatch-2026-10-28/baseline --bundle benchmarks/.build/gpu-progression-2026-10-28/acceptance/bundle --build-dir benchmarks/.build/gpu-dispatch-2026-10-28/counters-register --output benchmarks/results/transformer-dispatch-register-profile-2026-10-28.json --register-policy wide --processes 3','```']
stem.with_suffix('.md').write_text('\n'.join(lines)+'\n')
stem.with_suffix('.json').write_text(json.dumps(dict(evidence_sha256={name:sha(out/name) for name in names},positive_medians=wins,negative_medians=27-wins,all_positive_blocks=consistent,cpu_llvm_sha256=sha(cpu_baseline),cpu_baseline_evidence_sha256=sha(cpu_evidence),renderer_sha256=sha(Path(__file__))),indent=2)+'\n')
print(stem.with_suffix('.md'))
