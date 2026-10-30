#!/usr/bin/env python3
"""Build optimized native kernels/runtime; retain independently checked raw trials."""
import argparse
import datetime
import hashlib
import json
import pathlib
import platform
import statistics
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]


def run(command):
    return subprocess.check_output([str(x) for x in command], cwd=ROOT, text=True, timeout=180)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=pathlib.Path,
                        default=ROOT / 'build/native/native/kalandra')
    parser.add_argument('--clang', type=pathlib.Path)
    parser.add_argument('--output', type=pathlib.Path,
                        default=ROOT / f'benchmarks/results/native-cpu-{datetime.date.today().isoformat()}.json')
    parser.add_argument('--baseline', type=pathlib.Path,
                        help='Previously built scaling executable; alternate before/after trials')
    args = parser.parse_args()
    lock = json.loads((ROOT / 'native/toolchain-proof/toolchain.json').read_text())
    clang = args.clang or pathlib.Path(lock['defaultLlvmRoot']) / 'bin/clang++'
    build = ROOT / 'benchmarks/.build/native-cpu'
    build.mkdir(parents=True, exist_ok=True)
    ir = build / 'kernels.ll'
    binary = build / 'scaling'
    if args.baseline:
        args.baseline = args.baseline.resolve()
        if args.baseline == binary.resolve():
            parser.error('--baseline must be a saved copy, not the executable this run rebuilds')
        if not args.baseline.is_file():
            parser.error('--baseline executable does not exist')
    commands = [
        [args.compiler.resolve(), ROOT / 'native/tests/fixtures/dynamic-projection.k',
         '-o', build / 'projection', '--emit-llvm', ir, '--test-entry', 'reference', '-O2'],
        [clang, '-std=c++23', '-Wall', '-Wextra', '-Werror', '-O2', '-DNDEBUG',
         '-ffp-contract=off', '-Wno-override-module',
         ROOT / 'native/benchmarks/CpuScaling.cpp', ir,
         ROOT / 'native/runtime/Runtime.cpp', ROOT / 'native/runtime/Tasks.cpp', '-o', binary],
    ]
    start = time.monotonic()
    for command in commands:
        run(command)
    build_seconds = time.monotonic() - start
    sources = ['native/benchmarks/CpuScaling.cpp', 'native/benchmarks/run.py',
               'native/tests/fixtures/dynamic-projection.k', 'native/runtime/Runtime.cpp',
               'native/runtime/Runtime.h', 'native/runtime/Tasks.cpp', 'native/runtime/Tasks.h',
               'native/lib/Backend.cpp', 'native/lib/CheckedABI.cpp']
    hashes = {path: hashlib.sha256((ROOT / path).read_bytes()).hexdigest() for path in sources}
    trials = []
    baseline_trials = []
    for trial in range(3):
        variants = [(binary, trials)]
        if args.baseline:
            variants.insert(0, (args.baseline.resolve(), baseline_trials))
            if trial % 2:
                variants.reverse()
        for executable, destination in variants:
            print(f'Native CPU trial {trial + 1}/3: {executable.name}', flush=True)
            destination.append([json.loads(line) for line in run([executable]).splitlines()])

    def summarize(observations):
        summaries = []
        for dimension in (0, 8, 32, 96):
            for workers in (0, 1, 2, 4, 8):
                samples = [[r['ms'] for r in trial if r.get('dimension') == dimension
                            and r.get('workers') == workers and 'round' in r]
                           for trial in observations]
                medians = [statistics.median(s) for s in samples]
                summaries.append(dict(dimension=dimension, workers=workers,
                                      median_ms=statistics.median(medians),
                                      process_medians_ms=medians,
                                      sample_min_ms=min(map(min, samples)),
                                      sample_max_ms=max(map(max, samples))))
        return summaries

    summaries = summarize(trials)
    cpu = {}
    for key in ('machdep.cpu.brand_string', 'hw.logicalcpu', 'hw.perflevel0.logicalcpu',
                'hw.perflevel1.logicalcpu', 'hw.memsize'):
        try:
            cpu[key] = run(['sysctl', '-n', key]).strip()
        except subprocess.CalledProcessError:
            cpu[key] = 'unavailable'
    result = dict(timestamp=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  platform=platform.platform(), cpu=cpu, revision=run(['git', 'rev-parse', 'HEAD']).strip(),
                  worktree_status=run(['git', 'status', '--short']), source_sha256=hashes,
                  llvm_sha256=hashlib.sha256(ir.read_bytes()).hexdigest(),
                  compiler_sha256=hashlib.sha256(args.compiler.read_bytes()).hexdigest(),
                  clang=run([clang, '--version']).strip(),
                  build_commands=[[str(x) for x in c] for c in commands],
                  build_seconds=build_seconds, trials=trials, summary=summaries)
    result['binary_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
    if args.baseline:
        result['baseline'] = dict(path=str(args.baseline.resolve()),
                                  binary_sha256=hashlib.sha256(args.baseline.read_bytes()).hexdigest(),
                                  trials=baseline_trials, summary=summarize(baseline_trials))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    for row in summaries:
        print(f"n={row['dimension']:2} workers={row['workers']} median={row['median_ms']:.3f} ms")
    print(f'Raw results: {args.output}')


if __name__ == '__main__':
    main()
