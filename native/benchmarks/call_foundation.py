"""Paired native-call qualification against a frozen compiler and runtime."""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'fair'))
import protocol as p
import source_run as source


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline-compiler', type=Path, required=True)
    parser.add_argument('--baseline-runtime', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--depths', type=int, nargs='+', default=[9, 10, 11, 12, 13, 14])
    parser.add_argument('--modes', nargs='+', choices=['unbound', 'bound', 'observed'],
                        default=['unbound', 'bound', 'observed'])
    args = parser.parse_args()
    assert not args.output.exists() and not args.output.with_suffix('.md').exists()
    assert args.depths and len(set(args.depths)) == len(args.depths)
    assert all(0 <= depth <= 14 for depth in args.depths)
    assert len(set(args.modes)) == len(args.modes)
    compiler = p.ROOT / 'build/native/native/kalandra'
    runtime = compiler.parent / 'libkalandra-task-core-optimized.a'
    old = args.baseline_compiler.resolve()
    old_runtime = args.baseline_runtime.resolve()
    directory = p.BUILD.parent / 'call-foundation'
    directory.mkdir(exist_ok=True)
    src = Path(__file__).resolve().parent / 'fair'
    text = (src / 'SourceDriver.cpp').read_text()
    text = re.sub(r'#include "([^"]+)"', lambda m: '#include "' +
                  str((src / m[1]).resolve()) + '"', text)
    assert text.count('kalandra::runtime::Invocation context;') == 1
    drivers = {'unbound': src / 'SourceDriver.cpp'}
    for mode, setup in [('bound', 'std::atomic<bool> cancellation{false};\n      '
                         'context.bindCancellation(&cancellation);'),
                        ('observed', 'std::atomic<std::uint64_t> probes{0};\n      '
                         'context.watchPolls(&probes);')]:
        drivers[mode] = directory / (mode + '-driver.cpp')
        drivers[mode].write_text('#include <atomic>\n' + text.replace(
            'kalandra::runtime::Invocation context;',
            'kalandra::runtime::Invocation context;\n      ' + setup))
    dependencies = [p.ROOT / name for name in (
        'native/lib/Backend.cpp', 'native/lib/CheckedABI.cpp',
        'native/lib/NativeCallFoundation.h', 'native/lib/NativeFastABI.h',
        'native/lib/NativeStack.h', 'native/runtime/Runtime.h', 'native/runtime/Runtime.cpp')]
    inputs = [Path(__file__), src / 'protocol.py', src / 'source_run.py', src / 'source.k',
              *drivers.values(), *dependencies, old, old_runtime, compiler, runtime]
    frozen = {path: p.sha(path) for path in inputs}
    artifacts = []
    flags = ['-std=c++23', '-O2', '-ffp-contract=off', '-Wno-override-module',
             '-pthread', '-DKALANDRA_KERNEL']
    for version, executable, archive in [('baseline', old, old_runtime),
                                          ('current', compiler, runtime)]:
        for name in source.NAMES:
            ir = directory / (version + '-' + name + '.ll')
            p.run([executable, src / 'source.k', '--test-entry', name, '--emit-llvm',
                   ir, '-O2', '-o', ir.with_suffix('.standalone')])
            for mode in args.modes:
                binary = directory / (version + '-' + name + '-' + mode)
                p.run([p.CLANG, *flags, drivers[mode], ir, archive, '-o', binary])
                artifacts.append(binary)
            artifacts.append(ir)

    def invoke(cell, count, rounds, seed=17):
        version, name, mode, depth = cell
        kind = source.NAMES.index(name)
        return p.observations(p.run([directory / (version + '-' + name + '-' + mode),
                                     kind, depth, count, rounds, seed]),
                              source.expected(kind, depth, seed), count, rounds)

    checked = 0
    for version in ('baseline', 'current'):
        for name in source.NAMES:
            for mode in args.modes:
                for depth in (0, 1, 3, 9):
                    for seed in (1, 17, 31):
                        invoke([version, name, mode, depth], 1, 1, seed)
                        checked += 1
    print(f'{checked} independent reference workloads passed', flush=True)
    cells = [[version, name, mode, depth] for version in ('baseline', 'current')
             for name in source.NAMES for mode in args.modes for depth in args.depths]
    result = p.measure_paired(cells, invoke,
        lambda cell: source.expected(source.NAMES.index(cell[1]), cell[3]))
    result.update(verification_workloads=checked, baseline_compiler_sha256=p.sha(old),
                  baseline_runtime_sha256=p.sha(old_runtime),
                  contract='Identical source algorithms and driver, Clang -O2 and strict floating '
                  'order. Each compiler links its own frozen optimized runtime. Runtime layout '
                  'is unchanged. Invocation lifecycle, arithmetic checks, cancellation and physical '
                  'admission are included. The baseline retains its old logical limit; current '
                  'uses the new internal convention without that limit. All checksums match '
                  'independent Fraction/integer/GCD references. Observed counters are diagnostic '
                  'controls, not a promise of unchanged poll counts. This is a compiler/runtime '
                  'comparison, not a cross-language or GPU ranking.')
    assert all(p.sha(path) == digest for path, digest in frozen.items()), 'benchmark inputs changed'
    p.evidence(args.output, result, inputs, artifacts + [old, old_runtime, runtime], compiler)
    table = {tuple(row['cell']): row['median_ns'] / 1000 for row in result['summary']}
    lines = ['# Native call convention: paired comparison', '', result['contract'], '',
             'Nine adjacent ABBA/BAAB blocks per algorithm, mode and depth; fresh processes, '
             'two warmups and three measured batches. Raw paired observations, hashes and commands '
             'are retained in the adjacent JSON.', '',
             '| Algorithm | Mode | Depth | Before µs | After µs | Time change |',
             '| --- | --- | ---: | ---: | ---: | ---: |']
    for name in source.NAMES:
        for mode in args.modes:
            for depth in args.depths:
                before = table['baseline', name, mode, depth]
                after = table['current', name, mode, depth]
                lines.append(f'| {name} | {mode} | {depth} | {before:.3f} | {after:.3f} | '
                             f'{100 * (after / before - 1):+.1f}% |')
    args.output.with_suffix('.md').write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
