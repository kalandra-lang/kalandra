"""Audit entry-contract results using a closed-form affine-modular oracle."""
import argparse
import hashlib
import json
import math
import statistics
from pathlib import Path


def oracle(kind, seed, rounds):
    modulus = 1000003
    scale = pow(13, rounds, modulus)
    offset = 7 if kind == 0 else seed + 7
    shift = offset * (scale - 1) * pow(12, -1, modulus) % modulus
    values = range(seed, seed + 8) if kind == 0 else [seed]
    return sum((value * scale + shift) % modulus for value in values)


def audit(path, verify_artifacts):
    data = json.loads(path.read_text())
    for name, source in data['source_snapshots'].items():
        assert hashlib.sha256(source.encode()).hexdigest() == data['source_sha256'][name]
    if verify_artifacts:
        for name, digest in data['artifact_sha256'].items():
            assert hashlib.sha256(Path(name).read_bytes()).hexdigest() == digest, name
    for row in data['verification']:
        _, kind, rounds = row['cell']
        expected = oracle(kind, row['seed'], rounds)
        assert [s['round'] for s in row['samples']] == [-2, -1, 0]
        assert all(s['checksum'] == expected and math.isfinite(s['ns']) for s in row['samples'])
    samples = 0
    for study in [data, data['task_scope_diagnostic']]:
        for row in study['observations']:
            _, kind, rounds = row['cell']
            expected = oracle(kind, 17, rounds) * row['iterations']
            assert row['iterations'] > 0
            assert [s['round'] for s in row['samples']] == list(range(-2, 7))
            assert all(s['checksum'] == expected and math.isfinite(s['ns']) and s['ns'] > 0
                       for s in row['samples'])
            samples += len(row['samples'])
        for row in study['summary']:
            medians = [statistics.median(s['ns'] / r['iterations'] for s in r['samples'] if s['round'] >= 0)
                       for r in study['observations'] if r['cell'] == row['cell']]
            assert len(medians) == 3 and row['median_ns'] == statistics.median(medians)
            assert row['expected'] == oracle(row['cell'][1], 17, row['cell'][2])
    return dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                execution_contract=data['execution_contract'], verified=True,
                verification_workloads=len(data['verification']), timed_and_warmup_samples=samples,
                artifact_files_verified=verify_artifacts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', nargs='+', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--verify-artifacts', action='store_true')
    args = parser.parse_args()
    assert not args.output.exists()
    results = [audit(path, args.verify_artifacts) for path in args.inputs]
    args.output.write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
