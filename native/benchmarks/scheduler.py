#!/usr/bin/env python3
"""Separate uninstrumented scheduler phase timings from optional diagnostic probes."""
import argparse
import datetime
import hashlib
import itertools
import json
import pathlib
import platform
import random
import statistics
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILD = ROOT / 'benchmarks/.build/scheduler'


def run(command):
    return subprocess.check_output([str(x) for x in command], cwd=ROOT, text=True, timeout=180)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=pathlib.Path,
                        default=ROOT / f'benchmarks/results/native-scheduler-{datetime.date.today().isoformat()}.json')
    parser.add_argument('--baseline', type=pathlib.Path,
                        help='Saved timing executable for alternating before/after processes')
    parser.add_argument('--reference', type=pathlib.Path,
                        help='Optional older executable for a three-way regression comparison')
    args = parser.parse_args()
    if args.reference:
        args.reference = args.reference.resolve()
        if not args.baseline:
            parser.error('--reference requires --baseline')
        if (args.reference == (BUILD / 'timing').resolve() or not args.reference.is_file()
                or args.reference == args.baseline.resolve()):
            parser.error('--reference must be a distinct existing saved executable')
    if args.baseline:
        args.baseline = args.baseline.resolve()
        if args.baseline == (BUILD / 'timing').resolve() or not args.baseline.is_file():
            parser.error('--baseline must be an existing saved copy of the timing executable')
    BUILD.mkdir(parents=True, exist_ok=True)
    lock = json.loads((ROOT / 'native/toolchain-proof/toolchain.json').read_text())
    clang = pathlib.Path(lock['defaultLlvmRoot']) / 'bin/clang++'
    commands = []
    for name, extra in [('timing', []), ('profile', ['-DKALANDRA_TASK_PROFILE'])]:
        command = [clang, '-std=c++23', '-Wall', '-Wextra', '-Werror', '-O2', '-DNDEBUG', *extra,
                   ROOT / 'native/benchmarks/Scheduler.cpp', ROOT / 'native/runtime/Tasks.cpp',
                   ROOT / 'native/runtime/Runtime.cpp', '-o', BUILD / name]
        run(command)
        commands.append(list(map(str, command)))
    configs = list(itertools.product([1, 2, 4, 8], [(16, 1), (32, 1), (33, 1), (128, 1), (16, 32), (128, 32), (1024, 256)], [1, 8]))
    randomizer = random.Random(20261015)
    trials = []
    for trial in range(3):
        print(f'Scheduler trial {trial + 1}/3', flush=True)
        randomizer.shuffle(configs)
        for workers, (records, groups), hops in configs:
            arguments = [workers, records, groups, hops]
            observations = {}
            variants = [('current', BUILD / 'timing')]
            if args.baseline:
                variants.insert(0, ('baseline', args.baseline))
                if args.reference:
                    variants.insert(0, ('reference', args.reference))
                    variants = variants[trial:] + variants[:trial]
                elif trial % 2:
                    variants.reverse()
            for name, binary in variants:
                observations[name] = [json.loads(line) for line in run([binary, *arguments]).splitlines()]
            samples = observations['current']
            profile = json.loads(run([BUILD / 'profile', *arguments]))
            trials.append(dict(trial=trial, workers=workers, records=records, groups=groups,
                               hops=hops, samples=samples, profile=profile,
                               **({'baseline_samples': observations['baseline']} if args.baseline else {}),
                               **({'reference_samples': observations['reference']} if args.reference else {})))
    phases = ['admit_us', 'execute_us', 'observe_us', 'close_us', 'release_us']
    summary = []
    for workers, (records, groups), hops in sorted(configs):
        observations = [t for t in trials if (t['workers'], t['records'], t['groups'], t['hops'])
                        == (workers, records, groups, hops)]
        medians = [statistics.median(sum(s[p] for p in phases) for s in t['samples'])
                   for t in observations]
        row = dict(workers=workers, records=records, groups=groups, hops=hops,
                   median_us=statistics.median(medians), process_medians_us=medians)
        row['phases_us'] = {p: statistics.median(statistics.median(s[p] for s in t['samples'])
                                                for t in observations) for p in phases}
        for name, enabled in [('baseline', args.baseline), ('reference', args.reference)]:
            if not enabled:
                continue
            other_medians = [statistics.median(sum(s[p] for p in phases) for s in t[name + '_samples'])
                             for t in observations]
            row[name + '_median_us'] = statistics.median(other_medians)
            row[name + '_process_medians_us'] = other_medians
            row[name + '_phases_us'] = {
                p: statistics.median(statistics.median(s[p] for s in t[name + '_samples'])
                                     for t in observations) for p in phases}
        summary.append(row)
        print(f'{workers} workers {records}/{groups} slots {hops} resumes: {row["median_us"]:.2f} us')
    sources = ['native/benchmarks/Scheduler.cpp', 'native/benchmarks/scheduler.py',
               'native/runtime/Tasks.cpp', 'native/runtime/Tasks.h',
               'native/runtime/Runtime.cpp', 'native/runtime/Runtime.h']
    cpu = {}
    for key in ['machdep.cpu.brand_string', 'hw.logicalcpu', 'hw.memsize']:
        try:
            cpu[key] = run(['sysctl', '-n', key]).strip()
        except subprocess.CalledProcessError:
            cpu[key] = 'unavailable'
    output = args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(dict(timestamp=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                                     platform=platform.platform(), cpu=cpu,
                                     revision=run(['git', 'rev-parse', 'HEAD']).strip(),
                                     worktree_status=run(['git', 'status', '--short']),
                                     clang=run([clang, '--version']).strip(), commands=commands,
                                     binary_sha256=hashlib.sha256((BUILD / 'timing').read_bytes()).hexdigest(),
                                     baseline=(dict(path=str(args.baseline),
                                                    binary_sha256=hashlib.sha256(args.baseline.read_bytes()).hexdigest())
                                               if args.baseline else None),
                                     reference=(dict(path=str(args.reference),
                                                     binary_sha256=hashlib.sha256(args.reference.read_bytes()).hexdigest())
                                                if args.reference else None),
                                     source_sha256={p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest()
                                                    for p in sources}, trials=trials, summary=summary),
                                 indent=2) + '\n')
    print(output)


if __name__ == '__main__':
    main()
