"""Frozen before/after ordinary resumable calls; every invocation checked exactly."""
import argparse
import json
from pathlib import Path
import protocol as p

CASES = [('tail', 0), ('tail', 4), ('tail', 12), ('non-tail', 4), ('non-tail', 12), ('inline', 4)]

def source(kind, depth):
    prelude = 'from "std/time" import sleep, nanoseconds, TimeError\n'
    leaf = '  try sleep(nanoseconds(u64(0)))\n  return 17\n'
    if kind == 'inline':
        code = 'fn f0()->i64 ! TimeError:\n' + leaf
        for n in range(1, depth + 1):
            code += f'fn f{n}()->i64 ! TimeError:\n  return try f{n-1}()\n'
        call = f'f{depth}()'
    else:
        code = ('fn descend(n:i64)->i64 ! TimeError:\n  if n==0:\n'
                '    try sleep(nanoseconds(u64(0)))\n    return 17\n  return ' +
                ('1+' if kind == 'non-tail' else '') + 'try descend(n-1)\n')
        call = f'descend({depth})'
    return prelude + code + ('fn run()->i64:\n  return ' + call +
                             ' catch fn(error):return -1\nfn main()->i64:\n  return run()\n')

def expected(case):
    return 17 + (case[2] if case[1] == 'non-tail' else 0)

def main():
    q = argparse.ArgumentParser(description=__doc__)
    q.add_argument('--compiler', type=Path)
    q.add_argument('--headers', type=Path)
    q.add_argument('--build-dir', type=Path)
    q.add_argument('--baseline', type=Path)
    q.add_argument('--candidate', type=Path)
    q.add_argument('--output', type=Path, required=True)
    q.add_argument('--build-only', action='store_true')
    a = q.parse_args()
    if a.build_only:
        directory = a.build_dir.resolve()
        directory.mkdir(parents=True, exist_ok=True)
        assert not (directory / 'manifest.json').exists()
        compiler, headers = a.compiler.resolve(), a.headers.resolve()
        runtime = compiler.parent.parent / 'lib/kalandra/native/libkalandra-native-runtime.a'
        for kind, depth in CASES:
            folder = directory / f'{kind}-{depth}'
            folder.mkdir()
            path = folder / 'source.k'
            path.write_text(source(kind, depth))
            ir = folder / 'source.ll'
            p.run([compiler, path, '-O2', '--test-entry', 'run', '--emit-llvm', ir,
                   '-o', folder / 'standalone'])
            p.run([p.CLANG, '-std=c++23', '-O2', '-Wno-override-module', '-pthread',
                   '-iquote', headers, Path(__file__).with_name('ResumableCallDriver.cpp'),
                   ir, runtime, '-o', folder / 'driver'])
            assert p.run([folder / 'standalone']) == str(expected(['x', kind, depth])) + '\n'
            p.observations(p.run([folder / 'driver', 1, 1, expected(['x', kind, depth])]),
                           expected(['x', kind, depth]), 1, 1)
        files = [f for f in directory.rglob('*') if f.is_file()]
        files.extend((compiler, runtime, Path(__file__).resolve(),
                      Path(__file__).with_name('ResumableCallDriver.cpp').resolve(),
                      *headers.rglob('*.h')))
        (directory / 'manifest.json').write_text(json.dumps(dict(
            hashes={str(f): p.sha(f) for f in files}, commands=p.COMMANDS), indent=2) + '\n')
        return
    assert not a.output.exists()
    directories = dict(baseline=a.baseline.resolve(), current=a.candidate.resolve())
    manifests = {k: json.loads((v / 'manifest.json').read_text()) for k, v in directories.items()}
    def verify():
        for m in manifests.values():
            assert all(p.sha(f) == digest for f, digest in m['hashes'].items())
    verify()
    for kind, depth in CASES:
        assert (directories['baseline']/f'{kind}-{depth}'/'source.k').read_bytes() == (
            directories['current']/f'{kind}-{depth}'/'source.k').read_bytes()
    cells = [[label, kind, depth] for label in directories for kind, depth in CASES]
    def invoke(cell, count, rounds):
        path = directories[cell[0]] / f'{cell[1]}-{cell[2]}' / 'driver'
        return p.observations(p.run([path, count, rounds, expected(cell)]),
                              expected(cell), count, rounds)
    result = p.measure_paired(cells, invoke, expected, blocks=9, rounds=3,
                             target_ms=10, adaptive=True, max_iterations=1000000)
    verify()
    result.update(manifests=manifests, contract=(
        'Identical source and driver, each frozen production runtime and its own matching headers. '
        'One closed synchronous runner per process, one fresh receiver per invocation. '
        'No scheduling opt-out or loop elision flags. Exact scalar, fault-channel and checksum '
        'checks on warmup, calibration and timing calls. New deep-call capacities are correctness '
        'results, not speed comparisons against failing baselines.'))
    a.output.write_text(json.dumps(result, indent=2) + '\n')

if __name__ == '__main__':
    main()
