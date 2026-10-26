#!/usr/bin/env python3
"""Benchmark integrated native entries; no experimental rewrite or x28 harness."""
import argparse
import datetime
import json
import math
import pathlib
import random
import statistics
import sys
import entry_ablation as old

d = old.d
OUT = d.ROOT / 'benchmarks/.build/production-cancellation'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=pathlib.Path)
    parser.add_argument('--target-ms', type=float, default=20,
                        help='Shared per-cell calibration duration for every version')
    args = parser.parse_args()
    if not math.isfinite(args.target_ms) or args.target_ms <= 0:
        parser.error('--target-ms must be finite and positive')
    output = args.output
    assert not output.exists()
    OUT.mkdir(parents=True, exist_ok=True)
    d.build_references()
    compiler = d.ROOT / 'build/native/native/kalandra'
    runtime = d.ROOT / 'build/native/native/libkalandra-task-core-optimized.a'
    for kind, function in enumerate(d.NAMES):
        source = d.HERE / 'Depth.k' if kind == 3 else d.ROOT / 'native/benchmarks/fair/source.k'
        ir = OUT / (function + '.ll')
        d.p.run([compiler, source, '--test-entry', function, '--emit-llvm', ir,
                 '-O2', '-o', OUT / (function + '-standalone')])
        d.p.run([d.p.CLANG, '-std=c++23', '-O2', '-ffp-contract=off', '-Wno-override-module',
                 d.ROOT / 'native/benchmarks/fair/NativeEntry.cpp', ir, runtime, '-pthread',
                 '-o', OUT / function])

    def observe(name, kind, depth, count, seed=17):
        expected = float(d.exact(kind, depth, seed))
        if name in ('cpp', 'rust'):
            text = d.p.run([d.OUT / name, kind, depth, count, 1, seed])
            row = next(row for row in map(json.loads, text.splitlines()) if row['round'] == 0)
        else:
            row = json.loads(d.p.run([OUT / d.NAMES[kind], int(name), depth, count, seed, repr(expected)]))
        # Each invocation is checked against the Fraction oracle by the driver.
        # Accumulated checksums use the same rounding bound as the prior study.
        eps = 2**-53
        bound = (count * 2 * math.ulp(expected) +
                 ((count-1)*eps/(1-(count-1)*eps))*count*abs(expected) +
                 math.ulp(expected*count))
        assert abs(row['checksum'] - count*expected) <= bound
        return dict(row, version=name, kind=kind, depth=depth, count=count)

    versions = ('0', '1', '3', 'cpp', 'rust')
    verification = [observe(name, kind, depth, 1, seed) for name in versions for kind in range(4)
                    for depth in ((0, 1, 100, 1000, 10000) if kind == 3 else (0, 1, 5, 10, 14))
                    for seed in (1, 17, 31)]
    cells = [(kind, depth) for kind in range(3) for depth in (10, 14, 18, 22)]
    cells += [(3, depth) for depth in (0, 100, 1000, 10000)]
    counts = {(kind, depth): max(1, int(args.target_ms * 1000000 / max(1, observe('cpp', kind, depth, 1)['ns'])))
              for kind, depth in cells}
    rng = random.Random(20261025)
    samples = []
    for block in range(7):
        rng.shuffle(cells)
        for kind, depth in cells:
            count = min(100000, counts[kind, depth])
            order = list(versions)
            rng.shuffle(order)
            samples.extend(dict(block=block, **observe(name, kind, depth, count))
                           for name in order + order[::-1])
        print('Production entry block', block+1, 'complete', flush=True)
    summary = []
    for kind, depth in sorted(cells):
        times = {name: statistics.median(row['ns'] / row['count'] for row in samples
                                       if row['version'] == name and row['depth'] == depth and row['kind'] == kind)
                 for name in versions}
        row = dict(kind=d.NAMES[kind], depth=depth, median_ns=times, hybrid_over_cpp=times['3']/times['cpp'],
                   hybrid_over_rust=times['3']/times['rust'], hybrid_over_unbound=times['3']/times['0'])
        print(json.dumps(row), flush=True)
        summary.append(row)
    paths = (pathlib.Path(__file__), pathlib.Path(d.__file__), compiler, runtime,
             d.HERE / 'Depth.k', d.HERE / 'Depth.cpp',
             d.ROOT / 'native/benchmarks/fair/SourceDriver.cpp',
             d.ROOT / 'native/benchmarks/fair/source.k',
             d.ROOT / 'native/benchmarks/fair/source.rs',
             d.ROOT / 'native/benchmarks/fair/NativeEntry.cpp', d.OUT / 'cpp', d.OUT / 'rust',
             *[OUT / name for name in d.NAMES], *OUT.glob('*.ll'))
    output.write_text(json.dumps(dict(timestamp=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        contract='Real production compiler/runtime entry; no experimental LLVM preparation or callback harness. Modes 0 unbound, 1 cooperative token, 3 integrated hybrid boundary. Fresh invocation and ledger/status/depth/numerical checks timed. Two full-batch warmups, seven randomized mirrored blocks, ordinary C++/Rust reference kernels freshly rebuilt from the depth-study sources. This measures native entry lifecycle, not core creation/scheduler dispatch.',
        calibration_target_ms=args.target_ms, verification=verification, samples=samples, summary=summary, commands=d.p.COMMANDS,
        hashes={d.p.key(path): d.p.sha(path) for path in paths}), indent=2)+'\n')


if __name__ == '__main__':
    main()
