#!/usr/bin/env python3
"""Compare scalar/SIMD LLVM using identical rectangular workload and runtime builds."""
import argparse
import datetime
import hashlib
import json
import pathlib
import platform
import statistics
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILD = ROOT / 'benchmarks/.build/native-shapes'
WORKERS = (0, 1, 2, 4, 8)


def run(command):
    return subprocess.check_output(list(map(str, command)), cwd=ROOT, text=True, timeout=180)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def decode(output, rounds, jobs=16):
    records = [json.loads(line) for line in output.splitlines()]
    cases = {r['case']: r for r in records if 'expected_bits' in r}
    assert cases and len(cases) == sum('expected_bits' in r for r in records)
    for name, case in cases.items():
        assert case['allocations'] == case['releases']
        for workers in WORKERS:
            samples = [r for r in records if r['case'] == name and r.get('workers') == workers]
            assert len(samples) == rounds and {r['round'] for r in samples} == set(range(rounds))
            assert all(r['jobs'] == jobs and r['ms'] > 0 for r in samples)
    assert len(records) == len(cases) * (1 + len(WORKERS) * rounds)
    return records, cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-llvm', type=pathlib.Path, required=True,
                        help='Saved pre-SIMD LLVM; rebuilt with the same current harness/runtime')
    parser.add_argument('--compiler', type=pathlib.Path, default=ROOT / 'build/native/native/kalandra')
    parser.add_argument('--output', type=pathlib.Path,
                        default=ROOT / f'benchmarks/results/native-shapes-{datetime.date.today().isoformat()}.json')
    parser.add_argument('--suite', choices=['shapes', 'blocking'], default='shapes',
                        help='Original 37 shapes, or larger blocking rectangles with four-job batches')
    parser.add_argument('--sanitize-check', action='store_true',
                        help='Verify the full current suite under generated-code ASan and C++ ASan/UBSan first')
    args = parser.parse_args()
    args.baseline_llvm = args.baseline_llvm.resolve()
    if (not args.baseline_llvm.is_file() or
            args.baseline_llvm in (BUILD / 'current.ll', BUILD / 'sanitized.ll')):
        parser.error('--baseline-llvm must name an existing LLVM file outside current output paths')
    jobs, rounds = (4, 5) if args.suite == 'blocking' else (16, 9)
    BUILD.mkdir(parents=True, exist_ok=True)
    # Freeze the supplied IR before invoking the compiler or rebuilding executables.
    baseline_bytes = args.baseline_llvm.read_bytes()
    (BUILD / 'baseline.ll').write_bytes(baseline_bytes)
    lock = json.loads((ROOT / 'native/toolchain-proof/toolchain.json').read_text())
    clang = pathlib.Path(lock['defaultLlvmRoot']) / 'bin/clang++'
    sources = ['native/benchmarks/ShapeScaling.cpp', 'native/benchmarks/shapes.py',
               'native/tests/fixtures/dynamic-projection.k', 'native/runtime/Runtime.cpp',
               'native/runtime/Runtime.h', 'native/runtime/Tasks.cpp', 'native/runtime/Tasks.h',
               'native/lib/Backend.cpp', 'native/lib/CheckedABI.cpp']
    hashes = {p: digest(ROOT / p) for p in sources}
    commands = []

    def execute(command):
        commands.append(list(map(str, command)))
        return run(command)

    def generate(name, extra):
        execute([args.compiler.resolve(), ROOT / 'native/tests/fixtures/dynamic-projection.k',
                 '-o', BUILD / (name + '-projection'), '--emit-llvm', BUILD / (name + '.ll'),
                 '--test-entry', 'reference', *extra])

    def compile_variant(name, flags):
        execute([clang, '-std=c++23', '-Wall', '-Wextra', '-Werror', *flags,
                 '-ffp-contract=off', '-Wno-override-module',
                 *(['-DKALANDRA_BLOCKING_SHAPES'] if args.suite == 'blocking' else []),
                 ROOT / 'native/benchmarks/ShapeScaling.cpp',
                 BUILD / (name + '.ll'), ROOT / 'native/runtime/Runtime.cpp',
                 ROOT / 'native/runtime/Tasks.cpp', '-o', BUILD / name])

    generate('current', ['-O2'])
    for name in ('baseline', 'current'):
        compile_variant(name, ['-O2', '-DNDEBUG'])
    verification = {}
    expected = None
    for name in ('baseline', 'current'):
        print(f'Verifying {name} shape suite', flush=True)
        records, cases = decode(execute([BUILD / name, '--verify']), 1, jobs)
        signature = {key: {k: v for k, v in row.items() if k not in ('allocations', 'releases')}
                     for key, row in cases.items()}
        if expected is None:
            expected = signature
        assert signature == expected
        verification[name] = records
    if args.sanitize_check:
        generate('sanitized', ['--sanitize', '-O2'])
        flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                 '-fno-omit-frame-pointer']
        compile_variant('sanitized', flags)
        execute([clang, *flags, '-ffp-contract=off', '-Wno-override-module', '-S', '-emit-llvm',
                 BUILD / 'sanitized.ll', '-o', BUILD / 'instrumented.ll'])
        assert '__asan_report_load' in (BUILD / 'instrumented.ll').read_text()
        print('Verifying current shape suite with ASan/UBSan', flush=True)
        records, cases = decode(execute([BUILD / 'sanitized', '--verify']), 1, jobs)
        assert {key: {k: v for k, v in row.items() if k not in ('allocations', 'releases')}
                for key, row in cases.items()} == expected
        verification['sanitized'] = records
    # Sanitized verification and all compilation finish before performance trials.
    trials = []
    baseline_trials = []
    for trial in range(3):
        variants = [('baseline', baseline_trials), ('current', trials)]
        if trial % 2:
            variants.reverse()
        for name, destination in variants:
            print(f'Shape trial {trial + 1}/3: {name}', flush=True)
            records, cases = decode(execute([BUILD / name, '--seed', 20261015 + trial]), rounds, jobs)
            assert {key: {k: v for k, v in row.items() if k not in ('allocations', 'releases')}
                    for key, row in cases.items()} == expected
            destination.append(records)
    summary = []
    for name, case in sorted(expected.items()):
        for workers in WORKERS:
            row = dict(case, workers=workers)
            for label, observations in [('current', trials), ('baseline', baseline_trials)]:
                medians = [statistics.median(r['ms'] for r in trial
                                            if r['case'] == name and r.get('workers') == workers)
                           for trial in observations]
                row[label + '_median_ms'] = statistics.median(medians)
                row[label + '_process_medians_ms'] = medians
            row['speedup'] = row['baseline_median_ms'] / row['current_median_ms']
            summary.append(row)
    cpu = {}
    for key in ('machdep.cpu.brand_string', 'hw.logicalcpu', 'hw.perflevel0.logicalcpu',
                'hw.perflevel1.logicalcpu', 'hw.memsize'):
        try:
            cpu[key] = run(['sysctl', '-n', key]).strip()
        except subprocess.CalledProcessError:
            cpu[key] = 'unavailable'
    assert all(digest(ROOT / p) == h for p, h in hashes.items()), 'Sources changed during measurement'
    result = dict(schema_version=1, suite=args.suite, jobs=jobs, samples=rounds, timestamp=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  platform=platform.platform(), cpu=cpu, revision=run(['git', 'rev-parse', 'HEAD']).strip(),
                  worktree_status=run(['git', 'status', '--short']), source_sha256=hashes,
                  compiler_sha256=digest(args.compiler), clang=run([clang, '--version']).strip(),
                  baseline_llvm_path=str(args.baseline_llvm), baseline_llvm_sha256=hashlib.sha256(baseline_bytes).hexdigest(),
                  current_llvm_sha256=digest(BUILD / 'current.ll'),
                  binary_sha256={name: digest(BUILD / name) for name in ('baseline', 'current')},
                  commands=commands, verification=verification, cases=list(expected.values()),
                  trials=trials, baseline_trials=baseline_trials, summary=summary)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(expected)} shapes / {len(summary)} execution configurations verified and measured')
    print(args.output)


if __name__ == '__main__':
    main()
