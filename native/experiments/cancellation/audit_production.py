"""Audit production-entry results with independent closed-form/Fraction oracles."""
import argparse
import functools
import hashlib
import json
import math
import statistics
from fractions import Fraction
from pathlib import Path


@functools.cache
def exact(kind, depth, seed):
    count = 1 << depth if kind != 3 else 0
    first = seed * count
    if kind == 0:
        last = first + count - 1
        squares = lambda n: n * (n + 1) * (2 * n + 1) // 6
        return (Fraction(squares(last) - squares(first - 1), 8)
                + Fraction(count * (first + last), 8) - Fraction(count, 2))
    if kind == 1:
        result = Fraction(0)
        for residue in range(3):
            start = first + (residue - first) % 3
            n = max(0, (first + count - 1 - start) // 3 + 1)
            total = n * start + 3 * n * (n - 1) // 2
            result += [Fraction(total, 2), Fraction(-total, 4),
                       Fraction(total) + Fraction(n, 8)][residue]
        return result
    if kind == 2:
        # Euclid: gcd(13x+7,7x+11) = gcd(x+15,94), period 94.
        period = [math.gcd(x + 15, 94) for x in range(94)]
        cycles, tail = divmod(count, 94)
        return Fraction(cycles * sum(period)
                        + sum(period[(first + i) % 94] for i in range(tail)))
    values = [seed]
    for _ in range(depth):
        values.append((values[-1] * 13 + 7) % 1000003)
    result = Fraction(values[-1])
    for value in reversed(values[:-1]):
        result = result / 2 + Fraction(value, 4)
    return result


def audit(path, verify_artifacts):
    data = json.loads(path.read_text())
    versions = ('0', '1', '3', 'cpp', 'rust')
    expected_rows = [(version, kind, depth, seed)
                     for version in versions for kind in range(4)
                     for depth in ((0, 1, 100, 1000, 10000) if kind == 3 else (0, 1, 5, 10, 14))
                     for seed in (1, 17, 31)]
    assert len(data['verification']) == len(expected_rows) == 300
    for row, (version, kind, depth, seed) in zip(data['verification'], expected_rows):
        assert (row['version'], row['kind'], row['depth'], row['count']) == (version, kind, depth, 1)
        target = float(exact(kind, depth, seed))
        assert abs(row['checksum'] - target) <= 2 * math.ulp(target)
    for row in data['samples']:
        count = row['count']
        target = float(exact(row['kind'], row['depth'], 17))
        eps = 2**-53
        bound = (count * 2 * math.ulp(target)
                 + ((count - 1) * eps / (1 - (count - 1) * eps)) * count * abs(target)
                 + math.ulp(target * count))
        assert math.isfinite(row['ns']) and row['ns'] > 0 and count > 0
        assert abs(row['checksum'] - target * count) <= bound
    names = ('polynomial', 'branches', 'euclid', 'chain')
    assert len(data['samples']) == 1120
    for row in data['summary']:
        kind = names.index(row['kind'])
        times = {}
        for version in versions:
            samples = [s for s in data['samples'] if
                       (s['kind'], s['depth'], s['version']) == (kind, row['depth'], version)]
            assert len(samples) == 14
            assert sorted(s['block'] for s in samples) == sorted(list(range(7)) * 2)
            times[version] = statistics.median(s['ns'] / s['count'] for s in samples)
        assert times == row['median_ns']
        for reference in ('cpp', 'rust', 'unbound'):
            assert row['hybrid_over_' + reference] == times['3'] / times['0' if reference == 'unbound' else reference]
    if verify_artifacts:
        for name, digest in data['hashes'].items():
            assert hashlib.sha256(Path(name).read_bytes()).hexdigest() == digest, name
    return dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                independent_reference='Closed-form polynomial/residue sums, periodic Euclid reduction and exact Fraction chain',
                verification_workloads=300, timed_samples=1120,
                artifact_files_verified=verify_artifacts, verified=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reports', type=Path, nargs='+')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--verify-artifacts', action='store_true')
    args = parser.parse_args()
    assert not args.output.exists()
    records = [audit(path, args.verify_artifacts) for path in args.reports]
    args.output.write_text(json.dumps(records, indent=2) + '\n')
    print('Verified', len(records), 'production-entry reports')


if __name__ == '__main__':
    main()
