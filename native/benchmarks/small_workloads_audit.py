"""Recalculate paired pipeline evidence, including every warmup checksum."""
import json
import hashlib
import math
from pathlib import Path
import statistics
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / 'fair'))
import pipeline_reference as ref
import protocol as p

root = Path(__file__).resolve().parents[2]
expected = {}
records = []
for path in sorted((root/'benchmarks/results').glob('small-workloads-*-2026-10-26.json')):
    if path.name.startswith('small-workloads-evidence-audit-'):
        continue
    data = json.loads(path.read_text())
    for source, checksum in [('prototype_header_source', 'prototype_header_sha256'),
                             ('initial_runner_source', 'initial_runner_sha256')]:
        if source in data:
            assert hashlib.sha256(data[source].encode()).hexdigest() == data[checksum], path
    if 'paired' not in data and 'profiles' not in data:
        continue
    rows = data.get('paired', {}).get('observations', data.get('profiles', []))
    for row in rows:
        version, mode, n = row['cell']
        assert version in ('baseline', 'current') and mode in ('resident', 'transfers')
        if n not in expected:
            expected[n] = ref.exact(n)
        samples = row['samples']
        assert [s['round'] for s in samples] == list(range(-2, len(samples)-2)), path
        for sample in samples:
            assert math.isfinite(sample['ns']) and sample['ns'] > 0, path
            assert sample['checksum'] == expected[n] * row['iterations'], (path, row)
            assert ('gpu_ns' in sample) == ('profiles' in data), path
    for pair in data.get('paired', {}).get('pairs', []):
        group = [r for r in rows if (r['process'], r['cell'][1:]) ==
                 (pair['block'], pair['group'])]
        assert len(group) == 4 and [r['slot'] for r in group] == [0, 1, 2, 3], path
        order = ['baseline', 'current', 'current', 'baseline']
        if pair['block'] % 2:
            order = ['current', 'baseline', 'baseline', 'current']
        assert [r['cell'][0] for r in group] == order, path
        assert len({r['iterations'] for r in group}) == 1, path
        medians = {v: statistics.mean(statistics.median(s['ns']/r['iterations']
            for s in r['samples'] if s['round'] >= 0) for r in group if r['cell'][0] == v)
            for v in ('baseline', 'current')}
        for version in ('baseline', 'current'):
            assert math.isclose(pair[version+'_ns'], medians[version], abs_tol=1e-9), path
        assert math.isclose(pair['time_reduction'], 1-medians['current']/medians['baseline'], abs_tol=1e-12), path
        assert math.isclose(pair['speedup'], medians['baseline']/medians['current'], abs_tol=1e-12), path
    records.append(dict(path=str(path.relative_to(root)), sha256=p.sha(path),
                        processes=len(rows), pairs=len(data.get('paired', {}).get('pairs', []))))
assert records
print(json.dumps(dict(status='passed', records=records,
    processes=sum(r['processes'] for r in records), pairs=sum(r['pairs'] for r in records)), indent=2))
