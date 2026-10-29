"""Repeat the frozen encoder probe in sustained batches, never acceptance timing.

No build or shader changes: audit the earlier diagnostic's hashes and use its
same executable. Retain complete raw stdout/stderr in ignored experiment storage
and per-batch distributions plus their hashes in the tracked evidence.
"""
import argparse
import gzip
import hashlib
import json
import os
import random
import statistics as st
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'fair'))
import protocol as p

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--initial', type=Path, required=True)
parser.add_argument('--probe', type=Path, required=True)
parser.add_argument('--bundle', type=Path, required=True)
parser.add_argument('--raw', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
a = parser.parse_args()
assert not a.output.exists()
initial = json.loads(a.initial.read_text())
assert initial['acceptance'] is False
assert len(initial['verification']) == 8
assert all(row['bit_identical'] for row in initial['verification'])
for collection in ('source_sha256', 'artifact_sha256'):
    for file, digest in initial[collection].items():
        assert p.sha(p.ROOT / file) == digest, file
assert initial['artifact_sha256'][p.key(a.probe.resolve())] == p.sha(a.probe)
for flag in ('MTL_DEBUG_LAYER', 'MTL_SHADER_VALIDATION', 'DYLD_INSERT_LIBRARIES'):
    assert os.getenv(flag) in (None, '', '0'), flag

a.raw.mkdir(parents=True, exist_ok=True)
count, rounds = 128, 3
shapes = [(1, 64), (1, 512), (16, 512), (64, 512)]
rng = random.Random(291026)
records = []
for process in range(5):
    order = list(shapes)
    rng.shuffle(order)
    for m, d in order:
        command = list(map(str, [a.probe, a.bundle, m, d, count, rounds, 0, 0, 0, 2, 1]))
        raw = a.raw / f'{process}-{m}-{d}.json.gz'
        if raw.exists():
            with gzip.open(raw, 'rt') as stream:
                captured = json.load(stream)
            assert captured['command'] == command
        else:
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            assert result.returncode == 0, result.stderr[-2000:]
            captured = dict(command=command, stdout=result.stdout, stderr=result.stderr)
            with gzip.open(raw, 'wt') as stream:
                json.dump(captured, stream)
        samples = [json.loads(line) for line in captured['stderr'].splitlines()
                   if line.startswith('{"dispatch_counter_call":')]
        assert len(samples) == count * (rounds + 2)
        pcs = [9, 11, 13, 15] + ([] if m == 1 else [16]) + [18, 20, 22, 24, 27, 29, 32]
        for i, sample in enumerate(samples):
            assert sample['dispatch_counter_call'] == i
            assert [entry['pc'] for entry in sample['passes']] == pcs
            assert sample['nanoseconds_per_tick'] > 0
            assert all(entry['ns'] >= 0 and entry['end'] >= entry['start'] for entry in sample['passes'])
        observations = p.observations(captured['stdout'], m * d * 4, count, rounds)
        assert len(observations) == rounds + 2
        for row in observations:
            assert row['submissions'] == count
            assert row['dispatches'] == len(pcs) * count
            assert row['gpu_ns'] > 0 and row['encoding_ns'] > 0
        batches = []
        for batch in range(rounds):
            group = samples[(batch + 2) * count:(batch + 3) * count]
            passes = []
            for i, pc in enumerate(pcs):
                values = [sample['passes'][i]['ns'] for sample in group]
                passes.append(dict(pc=pc, median_ns=st.median(values), min_ns=min(values),
                                   max_ns=max(values), mean_ns=st.mean(values)))
            batches.append(dict(round=batch, passes=passes,
                command_gpu_ns=st.median(sample['command_gpu_ns'] for sample in group),
                summed_intervals_ns=st.median(sum(entry['ns'] for entry in sample['passes']) for sample in group),
                nanoseconds_per_tick=st.median(sample['nanoseconds_per_tick'] for sample in group)))
        records.append(dict(process=process, m=m, d=d, batches=batches,
                            observations=observations, raw=p.key(raw.resolve()), raw_sha256=p.sha(raw)))
        print(f'{process}: {m}x{d} sustained batches recorded', flush=True)
for collection in ('source_sha256', 'artifact_sha256'):
    for file, digest in initial[collection].items():
        assert p.sha(p.ROOT / file) == digest, file
p.evidence(a.output, dict(acceptance=False, contract=__doc__, count=count, warmup_calls=2*count,
    measured_calls=rounds*count, initial_evidence=str(a.initial), verification=initial['verification'],
    records=records), [Path(__file__), a.initial, p.ROOT / 'native/benchmarks/fair/protocol.py'],
    [a.probe, *a.bundle.glob('*.metallib')], p.ROOT / 'build/native/native/kalandra')
