"""Audit sustained encoder samples without turning them into acceptance times."""
import argparse
import gzip
import hashlib
import json
import statistics as st
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('input', type=Path)
parser.add_argument('--output', type=Path, required=True)
a = parser.parse_args()
data = json.loads(a.input.read_text())
assert data['acceptance'] is False
assert data['warmup_calls'] == 256 and data['measured_calls'] == 384
for collection in ('source_sha256', 'artifact_sha256'):
    for file, digest in data[collection].items():
        assert hashlib.sha256((ROOT / file).read_bytes()).hexdigest() == digest, file
labels = {9: 'Attention RMSNorm', 11: 'Q projection', 13: 'K projection',
          15: 'V projection', 16: 'K transpose', 18: 'QKᵀ + scale',
          20: 'Softmax', 22: 'Probabilities × V', 24: 'Output projection + residual',
          27: 'FFN RMSNorm', 29: 'FFN expansion + ReLU', 32: 'FFN contraction + residual'}
summary = []
for m, d in ((1, 64), (1, 512), (16, 512), (64, 512)):
    records = [r for r in data['records'] if (r['m'], r['d']) == (m, d)]
    assert len(records) == 5 and len({r['process'] for r in records}) == 5
    pcs = sorted(pc for pc in labels if m != 1 or pc != 16)
    passes = {pc: [] for pc in pcs}
    gpu, wall, shares = [], [], []
    for row in records:
        raw = ROOT / row['raw']
        assert hashlib.sha256(raw.read_bytes()).hexdigest() == row['raw_sha256']
        captured = json.load(gzip.open(raw, 'rt'))
        samples = [json.loads(line) for line in captured['stderr'].splitlines()
                   if line.startswith('{"dispatch_counter_call":')]
        assert len(samples) == 640
        batches = row['batches']
        assert [b['round'] for b in batches] == list(range(3))
        for b in batches:
            assert [v['pc'] for v in b['passes']] == pcs
            group = samples[(b['round'] + 2)*128:(b['round'] + 3)*128]
            assert st.median(s['command_gpu_ns'] for s in group) == b['command_gpu_ns']
            for index, entry in enumerate(b['passes']):
                values = [s['passes'][index]['ns'] for s in group]
                assert st.median(values) == entry['median_ns']
                assert min(values) == entry['min_ns'] and max(values) == entry['max_ns']
        values = {pc: st.median(b['passes'][index]['median_ns'] for b in batches)
                  for index, pc in enumerate(pcs)}
        for pc, value in values.items():
            passes[pc].append(value / 1000)
        shares.append(100 * (values[29] + values[32]) / sum(values.values()))
        gpu.append(st.median(b['command_gpu_ns'] for b in batches) / 1000)
        wall.append(st.median(r['ns'] / 128 for r in row['observations'] if r['round'] >= 0) / 1000)
    summary.append(dict(m=m, d=d, command_gpu_us=gpu, wall_us=wall, ffn_interval_share_percent=shares,
        passes=[dict(pc=pc, operation=labels[pc], process_medians_us=values, median_us=st.median(values))
                for pc, values in passes.items()]))
lines = ['# Sustained encoder diagnostic', '',
    'This repeats the exact frozen counter executable after the short-burst diagnostic. Five fresh processes per shape run 256 warmup calls and three batches of 128 measured calls. Shape order is shuffled. There are 12,800 calls in total; all per-batch resource, shape, dispatch and submission checks pass. The unchanged executable retains the original eight validated full-output checks; timed calls do not read every output cell.', '',
    'These are instrumented diagnostic times, not acceptance or a comparison with MPSGraph. Both the runtime profiler and encoder counters are enabled. Each interval can include scheduling and synchronization, and interval sums can overlap. Complete stdout/stderr is retained in compressed ignored experiment storage with hashes; the tracked evidence contains all per-batch distributions.', '',
    '| Tokens × width | Command GPU median µs | Process range µs | FFN share of summed intervals |',
    '|---|---:|---:|---:|']
for row in summary:
    g, f = row['command_gpu_us'], row['ffn_interval_share_percent']
    lines.append(f"| {row['m']} × {row['d']} | {st.median(g):.2f} | {min(g):.2f}–{max(g):.2f} | {min(f):.2f}–{max(f):.2f}% |")
row = summary[-1]
lines += ['', '## Largest-shape encoder intervals', '',
          '| Instruction | Operation | Median µs | Process range µs |', '|---:|---|---:|---:|']
for entry in sorted(row['passes'], key=lambda e: e['median_us'], reverse=True):
    values = entry['process_medians_us']
    lines.append(f"| {entry['pc']} | {entry['operation']} | {entry['median_us']:.2f} | {min(values):.2f}–{max(values):.2f} |")
lines += ['', '## Decision and limitations', '',
    'The large-shape command interval now varies by about 1.3% across process medians, instead of roughly threefold in the short-burst experiment. FFN expansion and contraction remain the largest combined target at 43.33–43.47% of summed median intervals. Sustaining activity reduces this observed variation; it does not prove whether frequency, startup state, competing activity or instrumentation caused the original drift.', '',
    'Next test FFN tile/data reuse while keeping per-cell arithmetic order. A kernel candidate still needs separate unprofiled paired/control/replication timings, independent numerical checks and whole-block/upload regressions. This diagnostic does not justify reassociation or a numerical-contract change, and does not show that the FFN owns 43% of application wall time.', '',
    'Two collector assertions failed before the completed sweep: one forgot that observation rows include two warmups; the other incorrectly expected profiling counters to be zero in this deliberately profiled executable. The first raw capture and both failure logs remain retained. The corrected collector validates warmups, exact dispatch counts and nonzero profiler values; it reused already captured valid samples rather than dropping slow outcomes. No runtime or shader was changed.', '',
    f'[Evidence]({a.input.name}) · [Unprofiled acceptance](gpu-closed-entry-2026-10-29.md) · [Short-burst diagnostic](gpu-dispatch-counter-summary-2026-10-29.md)']
a.output.with_suffix('.md').write_text('\n'.join(lines) + '\n')
a.output.with_suffix('.json').write_text(json.dumps(dict(acceptance=False, summary=summary,
    evidence_sha256=hashlib.sha256(a.input.read_bytes()).hexdigest()), indent=2) + '\n')
print(a.output.with_suffix('.md'))
