"""Audit and summarize frozen shape-specialization experiments, including losses."""
import csv
import hashlib
import json
import statistics as st
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT/'benchmarks/results'
evidence = {}
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def read(path):
    evidence[str(path.relative_to(ROOT))] = sha(path)
    return json.loads(path.read_text())
def audit(record):
    for field in ('source_sha256','artifact_sha256'):
        for name,digest in record[field].items(): assert sha(ROOT/name)==digest, name
def summarize(path, cells):
    data = read(path)
    audit(data)
    assert data['protocol']['blocks']==7
    assert len(data['observations'])==cells*7*4
    for row in data['observations']:
        assert [s['round'] for s in row['samples']]==[-2,-1,0,1,2]
        c=row['cell']
        expected=c[1]*c[3] if cells==6 else c[1]*c[2]*4
        assert all(s['checksum']==expected*row['iterations'] for s in row['samples'])
    grouped={}
    for row in data['pairs']:
        assert abs(row['time_reduction']-(1-row['current_ns']/row['baseline_ns']))<1e-12
        grouped.setdefault(tuple(row['group']),[]).append(row)
    assert len(grouped)==cells and all(len(v)==7 for v in grouped.values())
    load=read(path.with_name(path.stem+'-load.json'))
    loads=[r['external_cpu_percent'] for r in load['samples']]
    summary=[]
    for shape,rows in sorted(grouped.items()):
        values=[r['time_reduction']*100 for r in rows]
        summary.append(dict(shape=shape,time_reduction_percent=st.median(values),winning_blocks=sum(x>0 for x in values),blocks=values,baseline_us=st.median(r['baseline_ns']/1000 for r in rows),candidate_us=st.median(r['current_ns']/1000 for r in rows)))
    return dict(cells=summary,load=dict(samples=len(loads),minimum=min(loads),median=st.median(loads),maximum=max(loads)))
gui=read(OUT/'gpu-specialization-gui-2026-10-29.json')
qualifications=[]
counters={}
for mode in ('k','shape'):
    build=ROOT/f'benchmarks/.build/gpu-specialization-{mode}-2026-10-29'
    q=read(build/'qualification.json');audit(q)
    assert len(q['checks'])==72 and all(r['passed'] for r in q['checks'])
    capture=read(build/'capture.json');audit(capture)
    assert capture['qualification_sha256']==sha(build/'qualification.json')
    qualifications.append(dict(mode=mode,qualification=q,capture=capture))
    for path in sorted((build/'specialized.gputrace').rglob('*')):
        if path.is_file(): evidence[str(path.relative_to(ROOT))]=sha(path)
    path=OUT/f'gpu-specialization-{mode}-counters-2026-10-29.csv'
    evidence[str(path.relative_to(ROOT))]=sha(path)
    rows=list(csv.DictReader(path.open()));assert len(rows)==1 and rows[0]['Kernel Invocations']=='32768'
    counters[mode]=rows[0]
q=read(OUT/'gpu-shape-runtime-verification-2026-10-29.json');audit(q)
assert len(q['checks'])==117 and all(r['bit_identical'] for r in q['checks'])
isolated={name:summarize(OUT/f'gpu-specialization-{name}-paired-2026-10-29.json',6) for name in ('k','shape','shape-control','shape-replication')}
runtime={name:summarize(OUT/f'gpu-shape-runtime-{name}-2026-10-29.json',12) for name in ('frozen','control','ordinary','repeat')}
qout=OUT/'gpu-specialization-qualification-2026-10-29.json'
qout.write_text(json.dumps(dict(checks=144,failed=0,records=qualifications),indent=2)+'\n')
evidence[str(qout.relative_to(ROOT))]=sha(qout)
evidence[str(Path(__file__).resolve().relative_to(ROOT))]=sha(Path(__file__))
summary=dict(production_change=False,acceptance=False,isolated_checks=144,full_model_checks=117,compiler_observations=gui,encoder_counters=counters,isolated=isolated,whole_model=runtime,evidence_sha256=evidence)
(OUT/'gpu-specialization-2026-10-29.json').write_text(json.dumps(summary,indent=2)+'\n')
lines=['# Strict GPU shape specialization', '',
'**Experimental evidence, not a production promotion.** Apple M2 Max, 29 October 2026. Specialization preserves the original arithmetic and improves isolated kernels; the full transformer, upload controls, load noise and missing production gates are reported below.', '',
'## Changes and correctness', '',
'Two controls clone the existing register16 kernel: one fixes K with Metal function constants; the other fixes M/K/N. Arithmetic order, operand order, shared layout, 8 KiB allocation, barriers and 256-thread geometry remain unchanged. Strict floating point and no FMA contraction are retained. Pipeline creation is outside timed batches.', '',
'144 validation executions cover twelve aligned/odd shapes, three input patterns and baseline/candidate for both controls. Every output bit, unchanged input and output guard is checked. Mixed-scale finite and signed-zero data additionally use the independent CPU adjacent-pair oracle; exceptional values test compatibility with the existing GPU implementation, not independent IEEE conformance. Timed batches repeat these checks outside the clock.', '',
'The copied runtime prepares at most six extra PSOs per applicable plain/fused register16 entry at module load. Its six exact shapes are an experimental policy, not a proposed production whitelist. Original reflection validation is retained. Dispatch performs only an immutable bounded lookup; all other shapes and kernel selections retain generic fallbacks. No task-time compilation or cache growth is introduced. 117 complete-transformer cases match baseline bits and pass the independent oracle, across thirteen shapes, all three execution policies and all three upload modes.', '',
'## Actual resources', '',
'Xcode 27.1 GUI replay of the 64×2048×512 contraction. Compiler fields are manually transcribed; hardware occupancy comes from the exported encoder counters. Replay duration is not acceptance timing.', '',
'| Kernel | Allocated/high registers | Temporary | Spilled bytes | Theoretical occupancy | Measured occupancy |',
'|---|---:|---:|---:|---:|---:|',
'| Original | 100/100 | 52 | 0 | 29.167% | 24.70% |']
for row in gui['records']:
    mode=row['variant'];lines.append(f"| {mode} | {row['allocated_registers']}/{row['high_register']} | {row['temporary_registers']} | 0 | 35.417% | {counters[mode]['Kernel Occupancy']} |")
lines += ['',
'Both still report 80 static threadgroup-load instructions. Static ALU counts are 230 for K-only and 189 for full shape. The higher theoretical ceiling does **not** establish higher achieved occupancy: full-shape measured occupancy is lower than the original. Specialization also removes generic index/bounds/root-state work; these experiments do not isolate register allocation as the causal mechanism.', '',
'## Isolated paired results', '',
'Positive percentages mean lower elapsed time. Values are medians of seven adjacent ABBA/BAAB block ratios; parentheses show winning blocks. Two warmups, three measured rounds, identical paired batch counts, 30 ms calibration target capped at 256 calls. No samples are removed. The AA control executes the identical original kernel under both labels.', '',
'| Study | 16×512×512 | 16×512×2048 | 16×2048×512 | 64×512×512 | 64×512×2048 | 64×2048×512 |',
'|---|---:|---:|---:|---:|---:|---:|']
for name,record in isolated.items():
    lines.append('| '+name+' | '+' | '.join(f"{r['time_reduction_percent']:+.2f}% ({r['winning_blocks']}/7)" for r in record['cells'])+' |')
lines += ['', '## Whole transformer', '',
'Frozen and repeat compare the original runtime/bundle with the copied specialized runtime/bundle. Control uses the same copied executable for both labels, changing only the bundle. Ordinary uses the ordinary cancellable policy; the other studies use the existing closed proof. Seven ABBA/BAAB blocks, two warmups and three measured rounds; 50 ms calibration target, capped at 512 invocations. Full numerical output is qualified separately. Timed calls validate output shape, resource cleanup and submission count, not a value-derived checksum.', '',
'| M × width / uploads | Frozen | Same-runtime control | Ordinary | Replication |',
'|---|---:|---:|---:|---:|']
for i,cell in enumerate(runtime['frozen']['cells']):
    m,d,mode=cell['shape'];label=('resident','input','all weights')[mode]
    lines.append(f'| {m}×{d} / {label} | '+' | '.join(f"{runtime[n]['cells'][i]['time_reduction_percent']:+.2f}% ({runtime[n]['cells'][i]['winning_blocks']}/7)" for n in ('frozen','control','ordinary','repeat'))+' |')
lines += ['',
'Shapes 1×512 and 64×256 do not match the specialization set and serve as fallbacks. Negative values and inconsistent upload outcomes are retained. These results do not establish a universal transformer gain or vendor-library parity. Setup, PSO creation, compilation and uploads preceding the measured invocation are excluded; all-weight upload within an invocation is included in its corresponding mode.', '',
'## Load and acceptance limits', '',
'All builds, validation, capture, profiling and timing are sequential. Background applications remain open. Five-second process samples exclude the benchmark process tree; 100% represents one CPU core. Media analysis, WindowServer and Chrome contribute substantial activity. GPU idleness is not established. Replication and controls quantify some noise but cannot remove contention.', '',
'| Study | External CPU median | Maximum |', '|---|---:|---:|']
for name,row in [*isolated.items(),*[(f'runtime-{n}',r) for n,r in runtime.items()]]:
    lines.append(f"| {name} | {row['load']['median']:.1f}% | {row['load']['maximum']:.1f}% |")
lines += ['',
'No production compiler/runtime source changes. Cancellation boundaries, fault precedence, task ownership and resource budgets are not altered in production. The prototype has not passed cancellation/fault-injection qualification or a full native suite, and its additional PSO setup/memory cost is not accepted. Numerical screening alone is insufficient for promotion.', '',
'## Next architectural decision', '',
'Full shape knowledge is useful, but hard-coding benchmark shapes is not a language optimization. Before designing a general bounded specialization cache, test a simpler general representation: two-dimensional threadgroup coordinates can remove group division/modulo by runtime tile-column count. A separately guarded full-tile variant can remove boundary predicates without knowing exact dimensions. Neither proposal is measured here. If dynamic specialization remains necessary, bound preparation and cache ownership and keep expensive compilation outside cancellable execution; qualify cold/setup cost, fallback, cancellation and allocation-failure precedence.', '',
'## Reproduction', '',
'Use `shape_specialization_screen.py --mode k|shape --phase prepare --build NEW_DIR --output NEW_DIR/qualification.json`, then capture/measure with the same frozen build. `fair/specialization_control.py` supplies the AA control. Profile captures in Xcode and stop replay before timing.', '',
'Use `shape_specialization_prototype.py --build NEW_DIR`, then `shape_specialization_verify.py` and `shape_specialization_measure.py`. Measurement currently expects the dated verification record; update that explicit path for a new study. Run `python3 native/benchmarks/fair/specialization_report.py` to audit retained local artifacts and regenerate this report. Large captures/binaries remain ignored; small exports, qualifications, all observations and hashes are committed.', '',
'- [Audited summary and hashes](gpu-specialization-2026-10-29.json)',
'- [Isolated qualification](gpu-specialization-qualification-2026-10-29.json)',
'- [Full-model qualification](gpu-shape-runtime-verification-2026-10-29.json)',
'- [Compiler fields](gpu-specialization-gui-2026-10-29.json)',
'- [Prior rejected resource-guided experiments](gpu-resource-guided-2026-10-29.md)']
(OUT/'gpu-specialization-2026-10-29.md').write_text('\n'.join(lines)+'\n')
print('Audited 144 isolated + 117 full-output checks, 168 isolated + 336 full-model paired comparisons, all source/artifact hashes.')
