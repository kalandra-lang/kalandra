"""Audit general 2D dispatch experiments, retaining all controls and regressions."""
import csv
import hashlib
import json
import statistics as st
import subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'benchmarks/results'
evidence={}
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def read(p):
    evidence[str(p.relative_to(ROOT))]=sha(p)
    return json.loads(p.read_text())
def audit(d):
    for field in ('source_sha256','artifact_sha256'):
        for name,digest in d[field].items():
            path=ROOT/name
            if path.exists() and sha(path)==digest: continue
            # Original tracked sources may subsequently be promoted. Verify the
            # exact recorded revision, never silently accept the current file.
            assert field=='source_sha256' and not Path(name).is_absolute(),name
            blob=subprocess.check_output(['git','show',d['revision']+':'+name],cwd=ROOT)
            assert hashlib.sha256(blob).hexdigest()==digest,name

def timed(path,cells,qualification):
    d=read(path);audit(d)
    assert d.get('qualification_sha256',d.get('verification_sha256'))==sha(qualification)
    assert d['protocol']['blocks']==7 and len(d['observations'])==cells*28
    for row in d['observations']:
        c=row['cell'];expected=c[1]*c[3] if cells==6 else c[1]*c[2]*4
        assert [s['round'] for s in row['samples']]==[-2,-1,0,1,2]
        assert all(s['checksum']==expected*row['iterations'] for s in row['samples'])
    groups={}
    for r in d['pairs']:
        assert abs(r['time_reduction']-(1-r['current_ns']/r['baseline_ns']))<1e-12
        groups.setdefault(tuple(r['group']),[]).append(r)
    assert len(groups)==cells and all(len(v)==7 for v in groups.values())
    load=read(path.with_name(path.stem+'-load.json'))
    values=[r['external_cpu_percent'] for r in load['samples']]
    return dict(cells=[dict(shape=g,time_reduction_percent=st.median(r['time_reduction']*100 for r in rs),winning_blocks=sum(r['time_reduction']>0 for r in rs),blocks=[r['time_reduction']*100 for r in rs]) for g,rs in sorted(groups.items())],load=dict(median=st.median(values),maximum=max(values)))

quals=[];counters={}
for mode in ('grid','aligned'):
    b=ROOT/f'benchmarks/.build/gpu-{mode}-2026-10-29'
    q=read(b/'qualification.json');audit(q)
    assert len(q['checks'])==84 and all(r['passed'] for r in q['checks'])
    cap=read(b/'capture.json');audit(cap)
    assert cap['qualification_sha256']==sha(b/'qualification.json')
    for path in sorted((b/'grid.gputrace').rglob('*')):
        if path.is_file(): evidence[str(path.relative_to(ROOT))]=sha(path)
    quals.append(dict(mode=mode,qualification=q,capture=cap))
    path=OUT/f'gpu-grid-dispatch-{mode}-counters-2026-10-29.csv';evidence[str(path.relative_to(ROOT))]=sha(path)
    rows=list(csv.DictReader(path.open()));assert len(rows)==1 and rows[0]['Kernel Invocations']=='32768'
    counters[mode]=rows[0]
qpath=OUT/'gpu-grid-runtime-verification-2026-10-29.json';q=read(qpath);audit(q)
assert len(q['checks'])==117 and all(r['bit_identical'] for r in q['checks'])
isolated={mode:timed(OUT/f'gpu-grid-dispatch-{mode}-paired-2026-10-29.json',6,ROOT/f"benchmarks/.build/gpu-{mode.split('-')[0]}-2026-10-29/qualification.json") for mode in ('grid','aligned','aligned-control','grid-repeat','aligned-repeat')}
runtime={mode:timed(OUT/f'gpu-grid-runtime-{mode}-2026-10-29.json',12,qpath) for mode in ('frozen','control','ordinary','repeat')}
gui=read(OUT/'gpu-grid-dispatch-gui-2026-10-29.json')
qout=OUT/'gpu-grid-dispatch-qualification-2026-10-29.json';qout.write_text(json.dumps(dict(checks=168,records=quals),indent=2)+'\n');evidence[str(qout.relative_to(ROOT))]=sha(qout)
evidence[str(Path(__file__).resolve().relative_to(ROOT))]=sha(Path(__file__))
(OUT/'gpu-grid-dispatch-2026-10-29.json').write_text(json.dumps(dict(prototype_only=True,isolated=isolated,whole_model=runtime,compiler_observations=gui,encoder_counters=counters,evidence_sha256=evidence),indent=2)+'\n')
lines=['# General 2D GPU dispatch', '',
'Apple M2 Max, 29 October 2026. This section records the frozen experimental evidence; production qualification, if completed, is recorded separately below.', '',
'## Numerical and execution contract', '',
'Both variants use the original strict register16 arithmetic, reduction tree, shared tile, barriers and 256-thread geometry. A uint2 threadgroup coordinate replaces division/modulo by the runtime tile-column count. The lane uses thread_index_in_threadgroup, required for mixing a scalar lane index with vector grid coordinates. The aligned variant additionally removes output bounds predicates only for complete 16×16 tiles. No dimensions are function constants; no shape cache exists.', '',
'168 isolated validation executions cover fourteen shapes, three patterns and both versions. Full bits, input immutability and output guards pass; finite/signed-zero cases also match an independently rounded CPU tree. Exceptional values establish compatibility with the existing GPU kernel, not independent IEEE conformance. Isolated aligned-mode ragged cases use bounded 2D fallback. The whole-model prototype instead retains the original 1D fallback, because plain 2D did not establish a broad win.', '',
'117 complete-transformer cases pass exact baseline comparison and an independent oracle across thirteen shapes, three execution policies and three upload modes. The copied aligned-only runtime prepares one optional extra pipeline per eligible plain/fused entry at module load. It preserves other kernel choices, legacy fallback, command boundaries and task-time allocation behavior. Full cancellation/fault acceptance is separate from numerical verification.', '',
'## Resources', '',
'Xcode 27.1 full GUI replay of 64×2048×512. Compiler fields are manually transcribed, measured occupancy is from encoder exports. Replay times are not acceptance timings.', '',
'| Variant | Registers / high | Temporary | Spilled bytes | Theoretical occupancy | Measured occupancy | ALU instructions |', '|---|---:|---:|---:|---:|---:|---:|',
'| Original | 100/100 | 52 | 0 | 29.167% | 24.70% | — |']
for r in gui['records']:
    lines.append(f"| {r['variant']} | {r['allocated_registers']}/{r['high_register']} | {r['temporary_registers']} | 0 | {r['max_theoretical_occupancy_percent']}% | {counters[r['variant']]['Kernel Occupancy']} | {r['static_alu_instructions']} |")
lines+=['','All retain 80 static threadgroup-load instructions. Register, instruction and occupancy changes are observations, not an isolated causal attribution.','',
'## All isolated timings','',
'Positive percentages mean less elapsed time; parentheses are winning blocks out of seven. Adjacent ABBA/BAAB fresh processes, two warmups, three measured rounds, 30 ms calibration target capped at 256 calls. Every output and guard is checked outside timing. AA executes the identical original kernel under both labels. No samples were discarded.', '',
'| Study | 16×512×512 | 16×512×2048 | 16×2048×512 | 64×512×512 | 64×512×2048 | 64×2048×512 |','|---|---:|---:|---:|---:|---:|---:|']
for n,r in isolated.items():lines.append('| '+n+' | '+' | '.join(f"{c['time_reduction_percent']:+.2f}% ({c['winning_blocks']}/7)" for c in r['cells'])+' |')
lines+=['','The initial runs are highly variable and retained as contended screens. Replication stabilizes the baseline; AA medians remain within one percent. Plain 2D has little general benefit. Aligned tiles reproduce approximately 13–23% isolated reductions without an exact-shape whitelist.','',
'## Whole transformer','',
'Frozen/repeat compare the original executable/bundle with the copied aligned-only runtime/bundle. Control uses the same copied executable on both bundles. Ordinary retains the ordinary cancellable policy; other studies use the existing closed proof. Seven paired blocks, two warmups, three rounds; 50 ms target capped at 512 calls. Timed outputs check shape, resource cleanup and command count, not a numerical checksum; full numerical outputs are verified separately. Module load and pipeline compilation are excluded.', '',
'| M × width / uploads | Frozen | Same-runtime control | Ordinary | Repeat |','|---|---:|---:|---:|---:|']
for i,c in enumerate(runtime['frozen']['cells']):
    m,d,mode=c['shape'];label=('resident','input','all weights')[mode]
    lines.append(f'| {m}×{d} / {label} | '+' | '.join(f"{runtime[n]['cells'][i]['time_reduction_percent']:+.2f}% ({runtime[n]['cells'][i]['winning_blocks']}/7)" for n in ('frozen','control','ordinary','repeat'))+' |')
lines+=['','The 1×512 and 64×256 fallbacks are retained, including regressions. Upload results are not interchangeable with resident results. No new CPU/vendor-library comparison or general LLM parity is claimed.','',
'## Background load','',
'Builds, validation, profiling and timing were sequential. Process samples every five seconds exclude the benchmark tree; 100% is one CPU core. Media analysis, video services, WindowServer and other applications remain active. An idle GPU is not established. All adverse observations remain recorded.', '',
'| Study | External CPU median | Maximum |','|---|---:|---:|']
for n,r in [*isolated.items(),*[(f'runtime-{n}',r) for n,r in runtime.items()]]:lines.append(f"| {n} | {r['load']['median']:.1f}% | {r['load']['maximum']:.1f}% |")
lines+=['','## Reproduction and evidence','',
'`grid_dispatch_screen.py --mode grid|aligned --phase prepare|capture|measure --build DIR --output FILE` creates and checks frozen artifacts. `fair/grid_control.py` supplies AA. `grid_dispatch_prototype.py --mode aligned --build DIR` prepares the whole-model copy; `grid_dispatch_verify.py` verifies outputs, then `grid_dispatch_measure.py --verification FILE --build DIR --study frozen|control|ordinary|repeat --output FILE` measures it. `fair/grid_dispatch_report.py` audits all original sources/artifacts and regenerates this experimental report. Source versions changed by later promotion are checked against the exact recorded Git revision. Large captures remain ignored.', '',
'- [Audited summary and hashes](gpu-grid-dispatch-2026-10-29.json)',
'- [All isolated qualification](gpu-grid-dispatch-qualification-2026-10-29.json)',
'- [Whole-model verification](gpu-grid-runtime-verification-2026-10-29.json)',
'- [Compiler observations](gpu-grid-dispatch-gui-2026-10-29.json)']
(OUT/'gpu-grid-dispatch-2026-10-29.md').write_text('\n'.join(lines)+'\n')
print('Audited 285 correctness executions, 546 paired block comparisons and source/artifact identities.')
