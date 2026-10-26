"""Recompute graph benchmark checksums, paired medians and source digests."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics


def audit(path):
    data = json.loads(path.read_text())
    for name, source in data['source_snapshots'].items():
        assert hashlib.sha256(source.encode()).hexdigest() == data['source_sha256'][name], name
    expected = {}
    counts = {}
    measured = data.get('paired', {}).get('observations', [])
    records = measured + data.get('profiles', []) + data.get('calibration', [])
    for record in records:
        version, *case = record['cell']
        count = record['iterations']
        assert count > 0
        rows = record['samples']
        assert [s['round'] for s in rows] == [-2, -1, 0, 1, 2]
        for row in rows:
            assert row['ns'] > 0 and math.isfinite(row['ns'])
            value = expected.setdefault(tuple(case), row['expected'])
            assert row['expected'] == value and row['checksum'] == value * count
            assert row['completions'] == row['submissions']
            submissions = row['submissions'] // count
            assert row['submissions'] == submissions * count
            assert counts.setdefault(tuple(record['cell']), submissions) == submissions
            rows_count, n, layers, relu = case
            stages = layers * (2 if relu else 1)
            if version == 'baseline':
                assert submissions == stages + 1
            if version == 'current':
                # Acceptance cases are either small enough for full four-stage
                # batches or the deliberately ineligible 512-cubed control.
                wanted = (stages + 3) // 4 + 1 if rows_count * n * n <= 64*1024*1024 else stages + 1
                assert submissions == wanted
            assert row['peak_bytes'] >= row['resident_bytes']
            if 'profiles' in data:
                length, levels = rows_count*n, 0
                while length > 1:
                    length = (length+127)//128
                    levels += 1
                assert row['dispatches'] == (stages+levels)*count
                assert row['gpu_ns'] > 0 and row['encoding_ns'] > 0
    pairs = data.get('paired', {}).get('pairs', [])
    for pair in pairs:
        group, block = pair['group'], pair['block']
        selected = [r for r in measured if r['cell'][1:] == group and r['process'] == block]
        assert len(selected) == 4 and {r['slot'] for r in selected} == {0, 1, 2, 3}
        selected.sort(key=lambda r: r['slot'])
        assert [r['cell'][0] for r in selected] == (['baseline','current','current','baseline'] if block%2==0 else ['current','baseline','baseline','current'])
        assert len({r['iterations'] for r in selected}) == 1
        medians = {v:[statistics.median(s['ns']/r['iterations'] for s in r['samples'] if s['round']>=0) for r in selected if r['cell'][0]==v] for v in ('baseline','current')}
        before, after = statistics.mean(medians['baseline']), statistics.mean(medians['current'])
        assert pair['baseline_ns'] == before and pair['current_ns'] == after
        assert pair['speedup'] == before/after and pair['time_reduction'] == 1-after/before
    if measured:
        cells = {tuple(r['cell']) for r in measured}
        assert len(cells) == 28
        blocks = data['paired']['protocol']['blocks']
        assert len(pairs) == 14 * blocks and len(measured) == 56 * blocks
    return dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                measured_processes=len(measured), profiled_processes=len(data.get('profiles', [])),
                calibration_processes=len(data.get('calibration', [])), pairs=len(pairs),
                all_checks_passed=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('records', nargs='+', type=Path)
    args = parser.parse_args()
    print(json.dumps([audit(path) for path in args.records], indent=2))
