"""Audit strict resource-guided kernel experiments and retain all outcomes."""
import csv
import hashlib
import json
import statistics as st
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'benchmarks/results'
VARIANTS = {
    'phased32': 'gpu-register-lifetime-2026-10-29',
    'packed-b68': 'gpu-shared-load-2026-10-29',
    'swizzled-b64': 'gpu-swizzled-load-2026-10-29',
}
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
evidence = {}
def read(path):
    evidence[str(path.relative_to(ROOT))] = sha(path)
    return json.loads(path.read_text())
def audit(record):
    for field in ('source_sha256', 'artifact_sha256'):
        for name, digest in record[field].items():
            assert sha(ROOT / name) == digest, name

gui = read(OUT / 'gpu-resource-guided-gui-2026-10-29.json')
baseline_build = ROOT / 'benchmarks/.build/gpu-swizzled-load-2026-10-29'
baseline_rows = [json.loads(line) for line in (baseline_build/'baseline-capture.jsonl').read_text().splitlines()]
assert [r['round'] for r in baseline_rows] == [-2, -1, 0]
assert all(r['checksum'] == 32768 for r in baseline_rows)
for path in [OUT/'gpu-resource-guided-baseline-counters-2026-10-29.csv',baseline_build/'baseline-capture.jsonl',*sorted((baseline_build/'baseline.gputrace').rglob('*'))]:
    if path.is_file(): evidence[str(path.relative_to(ROOT))] = sha(path)
records, qualifications = [], []
for name, folder in VARIANTS.items():
    build = ROOT / 'benchmarks/.build' / folder
    qpath = build / 'qualification.json'
    q = read(qpath)
    assert len(q['checks']) == 72 and all(c['passed'] for c in q['checks'])
    audit(q)
    capture = read(build / 'capture.json')
    audit(capture)
    assert capture['qualification_sha256'] == sha(qpath)
    # Keep hashes of raw captures without checking their large files into Git.
    for path in sorted((ROOT / capture['trace']).rglob('*')):
        if path.is_file(): evidence[str(path.relative_to(ROOT))] = sha(path)
    datafile = OUT / f'gpu-resource-guided-{name}-paired-2026-10-29.json'
    data = read(datafile)
    audit(data)
    assert data['qualification_sha256'] == sha(qpath)
    assert data['protocol']['blocks'] == 7
    assert len(data['observations']) == 6 * 7 * 4
    for row in data['observations']:
        assert [s['round'] for s in row['samples']] == [-2, -1, 0, 1, 2]
        assert all(s['checksum'] == row['cell'][1] * row['cell'][3] * row['iterations'] for s in row['samples'])
    grouped = {}
    for pair in data['pairs']:
        assert abs(pair['time_reduction'] - (1 - pair['current_ns']/pair['baseline_ns'])) < 1e-12
        grouped.setdefault(tuple(pair['group']), []).append(pair['time_reduction']*100)
    assert len(grouped) == 6 and all(len(v) == 7 for v in grouped.values())
    load = read(datafile.with_name(datafile.stem + '-load.json'))
    loads = [r['external_cpu_percent'] for r in load['samples']]
    counters = OUT / f'gpu-resource-guided-{name}-counters-2026-10-29.csv'
    evidence[str(counters.relative_to(ROOT))] = sha(counters)
    rows = list(csv.DictReader(counters.open()))
    assert len(rows) == 1 and rows[0]['Kernel Invocations'] == '32768'
    records.append(dict(variant=name,shapes=[dict(shape=key,time_reduction_percent=st.median(v),winning_blocks=sum(x>0 for x in v),blocks=v) for key,v in sorted(grouped.items())],load=dict(samples=len(loads),minimum=min(loads),median=st.median(loads),maximum=max(loads)),encoder_counters=rows[0]))
    qualifications.append(dict(variant=name,qualification=q,capture=capture))
qout = OUT / 'gpu-resource-guided-qualification-2026-10-29.json'
qout.write_text(json.dumps(dict(checks=216,failed=0,records=qualifications),indent=2)+'\n')
evidence[str(qout.relative_to(ROOT))] = sha(qout)
evidence[str(Path(__file__).resolve().relative_to(ROOT))] = sha(Path(__file__))
summary = OUT / 'gpu-resource-guided-2026-10-29.json'
summary.write_text(json.dumps(dict(production_change=False,acceptance=False,checks=216,variants=records,compiler_observations=gui,evidence_sha256=evidence),indent=2)+'\n')
lines = ['# Resource-guided strict GPU kernel experiments', '',
'**No production candidate is accepted.** Three isolated controls preserve arithmetic but fail the broad performance gate. Apple M2 Max, 29 October 2026. The existing register16 kernel remains in production.', '',
'## What changed', '',
'- **phased32:** evaluate the two 32-leaf subtrees sequentially in a non-unrolled loop. The shared tile still contains 64 leaves and retains both barriers. This differs from the previously rejected 32-leaf tile.',
'- **packed-b68:** transpose the right-hand shared tile and pad its column stride to 68 floats. Products and their reduction tree are unchanged. Shared storage grows from 8,192 to 8,448 bytes.',
'- **swizzled-b64:** transpose with a four-float XOR permutation instead of padding, preserving the original 8,192-byte allocation. This is a storage-preserving control, not a pure padding ablation: address arithmetic also changes.', '',
'All use one output per thread, the same 16×16 tile, 256 threads, 64-leaf chunks, two barriers, global matrix layout and outer reduction tree. No FMA or reassociation is introduced.', '',
'## Direct resource measurements', '',
'Xcode 27.1 full GUI replay of the qualified 64×2048×512 contraction. Register fields come from the Shaders table; temporary registers, theoretical occupancy and load instructions come from the Pipeline State sidebar. These are manually transcribed compiler fields, distinct from exported hardware counters. Replay durations are not acceptance timings.', '',
'| Variant | Allocated / high registers | Temporary registers | Spilled bytes | Theoretical occupancy | Static TG loads | Measured occupancy |',
'|---|---:|---:|---:|---:|---:|---:|']
for entry in gui['records']:
    name = entry['variant']
    row = next((r for r in records if r['variant']==name),None)
    occupancy = row['encoder_counters']['Kernel Occupancy'] if row else entry.get('measured_occupancy','See baseline report')
    lines.append(f"| {name} | {entry['allocated_registers']} / {entry['high_register']} | {entry['temporary_registers']} | {entry['spilled_bytes']} | {entry['max_theoretical_occupancy_percent']}% | {entry['threadgroup_load_instructions']} | {occupancy} |")
lines += ['',
'The phased body executes twice per chunk: 40 static load instructions do not mean half the dynamic load work. Both transposed layouts reduce static load instructions, but all three candidate encoder exports still report 150,994,944 threadgroup bytes read, the same as the earlier baseline. Wider loads are not fewer transferred bytes. All remain spill-free; increased register counts do not establish a spilling explanation. Measured occupancy falls despite an unchanged reported ceiling. These observations do not isolate register allocation, bank behavior or instruction scheduling as the sole cause.', '',
'## All paired timings', '',
'Positive percentages mean less elapsed time; parentheses show winning blocks out of seven. Adjacent ABBA/BAAB fresh processes use identical calibrated batch counts, two warmups and three measured rounds; 30 ms target, maximum 256 calls. Every output and guard is checked outside timing. No samples were discarded.', '',
'| Variant | 16×512×512 | 16×512×2048 | 16×2048×512 | 64×512×512 | 64×512×2048 | 64×2048×512 |',
'|---|---:|---:|---:|---:|---:|---:|']
for row in records:
    lines.append('| '+row['variant']+' | '+' | '.join(f"{c['time_reduction_percent']:+.2f}% ({c['winning_blocks']}/7)" for c in row['shapes'])+' |')
lines += ['',
'The padded layout has two approximately 8% small-shape wins, retained here. They are insufficient for promotion: the other four shapes regress, and no independent replication or whole-model qualification establishes a useful specialization. The swizzle does not recover broad performance. Do not attribute the padded regression solely to its extra storage.', '',
'## Correctness and load limits', '',
'216 passing validation executions: three controls × twelve shapes × three patterns × baseline/candidate, with Metal API and shader validation enabled. Every result bit, input immutability and output guard is checked. Finite mixed-scale and signed-zero patterns additionally use an independently rounded CPU adjacent-pair oracle. NaN/infinity/subnormal patterns check compatibility with the existing GPU kernel, not independent IEEE conformance. Timed, calibration and capture runs repeat output checks without validation instrumentation during timing.', '',
'Builds, validation, captures, replay profiling and timing run sequentially. Background applications remain open; five-second external CPU samples exclude the benchmark process tree. 100% means one CPU core, not the whole machine. These are monitored screens, not a claim of an entirely idle GPU.', '',
'| Variant | Samples | External CPU minimum | Median | Maximum |',
'|---|---:|---:|---:|---:|']
for row in records:
    v=row['load'];lines.append(f"| {row['variant']} | {v['samples']} | {v['minimum']:.1f}% | {v['median']:.1f}% | {v['maximum']:.1f}% |")
lines += ['',
'Chrome, WindowServer, metadata indexing and other applications contribute to recorded background activity. Small percentage differences are not a clean acceptance claim. The broad regressions and resource failures are retained rather than retested until they disappear.', '',
'No compiler/runtime/numerical policy, cancellation boundary, fault precedence, ownership or budget behavior changes. No full native suite is claimed for these isolated benchmark-only experiments. The large raw captures and binaries remain ignored; small exports, hashes, qualification and all timing samples are retained.', '',
'## Next architectural test', '',
'Keep the current general kernel. A more useful next control is compile-time specialization of known contraction dimensions: determine whether eliminating generic bounds and unused reduction-root state lowers actual register demand while preserving the exact addition tree. That needs a generic fallback, bounded pipeline caching and whole-model/upload acceptance. It is a hypothesis, not a measured gain. Another address-layout sweep alone is not justified by these results.', '',
'## Reproduction and evidence', '',
'Use `register_lifetime_screen.py`, `shared_load_screen.py` or `swizzled_load_screen.py` with `--phase prepare --build NEW_DIR --output NEW_DIR/qualification.json`; then `--phase capture` or `--phase measure` with the same build and unused output path. Source and artifact manifests must match. Profile captures through Xcode GUI; stop replay before measuring. Run `python3 native/benchmarks/fair/resource_guided_report.py` to audit preserved local artifacts and regenerate this report.', '',
'- [Compiler fields](gpu-resource-guided-gui-2026-10-29.json)',
'- [All qualification](gpu-resource-guided-qualification-2026-10-29.json)',
'- [Audited summary and hashes](gpu-resource-guided-2026-10-29.json)',
'- [Matching baseline export](gpu-resource-guided-baseline-counters-2026-10-29.csv)',
'- [Earlier baseline and quad resources](gpu-m2-resources-2026-10-29.md)']
for name in VARIANTS:
    for suffix in ('paired-2026-10-29.json','paired-2026-10-29-load.json','counters-2026-10-29.csv'):
        f=f'gpu-resource-guided-{name}-{suffix}';lines.append(f'- [{name}: {suffix}]({f})')
(OUT/'gpu-resource-guided-2026-10-29.md').write_text('\n'.join(lines)+'\n')
print('Audited 216 qualification cases, all source/artifact hashes, and 126 paired comparisons.')
